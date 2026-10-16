#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <variant>

#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

#include "client/websocket_client_state.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

// The time left of a budget that started at `started_ms`. Subtracting the
// elapsed time from the budget cannot overflow, unlike adding a configured
// budget to a timestamp.
std::chrono::milliseconds websocket_client_state::heartbeat_delay(std::int64_t now) const noexcept {
    const auto remaining = [now](std::int64_t started_ms, std::int64_t budget) noexcept {
        const auto elapsed = now - started_ms;
        return elapsed >= budget ? std::int64_t{0} : budget - std::max<std::int64_t>(elapsed, 0);
    };
    std::int64_t delay_value = 1;
    const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_);
    if (pong != nullptr && write_phase_ == write_phase_type::idle) {
        delay_value = remaining(pong->sent_at_ms(), config_.heartbeat_.pong_timeout_->count());
    } else if (!std::holds_alternative<websocket_awaiting_peer_close>(liveness_state_)) {
        delay_value = remaining(last_active_ms_, config_.heartbeat_.ping_interval_->count());
        if (write_phase_ != write_phase_type::idle) {
            // The write holding the lane (an application message or the Ping
            // itself) has the Pong timeout on top of the Ping interval before
            // it counts as stalled.
            const auto pong_timeout = config_.heartbeat_.pong_timeout_->count();
            delay_value = pong_timeout > std::numeric_limits<std::int64_t>::max() - delay_value
                              ? std::numeric_limits<std::int64_t>::max()
                              : delay_value + pong_timeout;
        }
    }
    return std::chrono::milliseconds(std::max<std::int64_t>(1, delay_value));
}

void websocket_client_state::arm_heartbeat_timer(std::chrono::milliseconds delay) {
    heartbeat_timer_.cancel();
    // close_now() cancels this state-owned timer before retirement.
    (void)worker_.schedule_timer(heartbeat_timer_, worker_timer_deadline_after(std::max(delay, std::chrono::milliseconds{1})), [this](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired) {
            heartbeat_timer_fired();
        }
    });
}

void websocket_client_state::touch_activity() noexcept {
    last_active_ms_ = steady_now_ms();
    if (!protocol_open() || !config_.heartbeat_.ping_interval_.has_value() ||
        !std::holds_alternative<websocket_liveness_idle>(liveness_state_)) {
        return;
    }
    try {
        arm_heartbeat_timer(*config_.heartbeat_.ping_interval_);
    } catch (...) {
        fail_transport();
    }
}

// Called when a reader can poll again: a read begins, or the write lane that
// blocked the reader frees. A window that expired while nothing could observe
// the Pong restarts; an open one simply has its timer re-armed to its deadline.
void websocket_client_state::resume_pong_deadline() noexcept {
    const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_);
    if (pong == nullptr || !protocol_open() || !read_active_ || write_phase_ != write_phase_type::idle) {
        return;
    }
    const auto now = steady_now_ms();
    if (now - pong->sent_at_ms() >= config_.heartbeat_.pong_timeout_->count()) {
        liveness_state_ = websocket_awaiting_pong(now, pong->challenge());
    }
    try {
        arm_heartbeat_timer(heartbeat_delay(now));
    } catch (...) {
        fail_transport();
    }
}

bool websocket_client_state::heartbeat_ping_due() noexcept {
    const auto now = steady_now_ms();
    const websocket_lifecycle_options options{
        .heartbeat_ = config_.heartbeat_, .close_handshake_timeout_ = config_.close_handshake_timeout_};
    // A write holding the lane defers the Ping; a stalled write is aborted
    // once it exceeds the Ping interval plus the Pong timeout. Only a reader
    // that can poll (one is running and no write holds the lane) observes a
    // Pong, so the Pong window counts only then; resume_pong_deadline()
    // restarts an expired window when polling becomes possible again.
    const auto decision = evaluate_websocket_liveness(options, require_protocol().liveness_mode(), liveness_state_,
        write_phase_ != write_phase_type::idle, read_active_ && write_phase_ == write_phase_type::idle,
        last_active_ms_, now);
    if (decision == websocket_liveness_decision::send_ping) {
        return true;
    }
    if (decision == websocket_liveness_decision::abort_transport) {
        retire(client_abort_reason::timeout);
        return false;
    }
    if (!read_active_ && write_phase_ == write_phase_type::idle &&
        std::holds_alternative<websocket_awaiting_pong>(liveness_state_)) {
        // Nothing can observe the Pong: no timer until a read begins.
        return false;
    }
    try {
        arm_heartbeat_timer(heartbeat_delay(now));
    } catch (...) {
        fail_transport();
    }
    return false;
}

void websocket_client_state::heartbeat_timer_fired() noexcept {
    // A heartbeat in flight sends its Ping once the write lane is idle, so a
    // due Ping spawns nothing. The evaluation still runs: a Ping write stalled
    // by a peer that stopped reading is aborted like any other stalled write.
    if (!protocol_open() || !heartbeat_ping_due() || heartbeat_in_flight_) {
        return;
    }

    heartbeat_in_flight_ = true;
    try {
        heartbeat_tasks_.spawn(heartbeat());
    } catch (...) {
        heartbeat_in_flight_ = false;
        fail_transport();
    }
}

task<void> websocket_client_state::heartbeat() {
    try {
        // A due ping waits for an in-progress application write on the same
        // signal as every other writer. The write may have refreshed activity or
        // the connection may have closed meanwhile, so liveness is evaluated
        // again once the write lane is idle.
        co_await wait_for_write_idle(write_wait_type::writer);
        if (protocol_open() && heartbeat_ping_due()) {
            write_guard_type write_guard(*this, write_phase_type::heartbeat);
            const auto challenge = ++heartbeat_sequence_;
            liveness_state_ = websocket_sending_ping(challenge);
            const auto payload_value = websocket_heartbeat_payload(challenge);
            const auto submitted = require_protocol().submit_frame(websocket_opcode::ping,
                std::string_view(payload_value.data(), payload_value.size()));
            if (submitted != websocket_frame_submit_status::accepted) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "failed to submit WebSocket client heartbeat");
            }
            co_await flush_output();
            const auto ping_sent_at_ms = steady_now_ms();
            if (protocol_open() && std::holds_alternative<websocket_sending_ping>(liveness_state_)) {
                liveness_state_ = websocket_awaiting_pong(ping_sent_at_ms, challenge);
                arm_heartbeat_timer(*config_.heartbeat_.pong_timeout_);
            }
        }
    } catch (...) {
        // No caller observes the heartbeat: its failure retires the connection
        // and in-flight operations observe the closing reason.
        fail_transport();
    }
    heartbeat_in_flight_ = false;
}

}  // namespace ruvia::detail
