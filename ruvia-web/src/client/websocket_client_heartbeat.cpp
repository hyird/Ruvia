#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_timer.h"

#include "client/websocket_client_state.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

std::chrono::milliseconds websocket_client_state::heartbeat_delay(std::int64_t now) const noexcept {
    std::int64_t deadline_value = now + 1;
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_)) {
        deadline_value = pong->sent_at_ms() + config_.heartbeat_.pong_timeout_->count();
    } else if (std::holds_alternative<websocket_liveness_idle>(liveness_state_)) {
        deadline_value = last_active_ms_ + config_.heartbeat_.ping_interval_->count();
    }
    return std::chrono::milliseconds(std::max<std::int64_t>(1, deadline_value - now));
}

void websocket_client_state::arm_heartbeat_timer(std::chrono::milliseconds delay) {
    if (!config_.heartbeat_.ping_interval_.has_value()) {
        return;
    }
    heartbeat_timer_.cancel();
    std::weak_ptr<websocket_client_state> weak = shared_from_this();
    (worker_).schedule_timer(heartbeat_timer_, worker_timer_deadline_after(std::max(delay, std::chrono::milliseconds{1})), [weak = std::move(weak)](worker_timer_outcome outcome) noexcept {
        if (outcome != worker_timer_outcome::expired) {
            return;
        }
        if (const auto state = weak.lock()) {
            state->heartbeat_timer_fired();
        }
    });
}

void websocket_client_state::touch_activity() noexcept {
    last_active_ms_ = websocket_steady_now_ms();
    if (phase_.load(std::memory_order_acquire) != phase_type::open ||
        !config_.heartbeat_.ping_interval_.has_value() ||
        !std::holds_alternative<websocket_liveness_idle>(liveness_state_)) {
        return;
    }
    try {
        arm_heartbeat_timer(*config_.heartbeat_.ping_interval_);
    } catch (...) {
        close_on_worker(abort_reason_type::closing);
    }
}

bool websocket_client_state::heartbeat_ping_due() noexcept {
    const auto now = websocket_steady_now_ms();
    const websocket_lifecycle_options options{
        .heartbeat_ = config_.heartbeat_, .close_handshake_timeout_ = config_.close_handshake_timeout_};
    // An active write does not defer the decision here. heartbeat_owned() waits
    // for the write lane on write_signal_, so a due ping never re-arms the timer
    // just to poll for write completion.
    const auto decision = evaluate_websocket_liveness(
        options, require_protocol().liveness_mode(), liveness_state_, false, last_active_ms_, now);
    if (decision == websocket_liveness_decision::send_ping) {
        return true;
    }
    if (decision == websocket_liveness_decision::abort_transport) {
        close_on_worker(abort_reason_type::timeout);
        return false;
    }
    try {
        arm_heartbeat_timer(heartbeat_delay(now));
    } catch (...) {
        close_on_worker(abort_reason_type::closing);
    }
    return false;
}

void websocket_client_state::heartbeat_timer_fired() noexcept {
    // An in-flight heartbeat re-evaluates liveness and re-arms the timer when it
    // finishes waiting or writing, so a concurrent expiry has nothing to do.
    if (heartbeat_in_flight_ || phase_.load(std::memory_order_acquire) != phase_type::open ||
        !config_.heartbeat_.ping_interval_.has_value() || !heartbeat_ping_due()) {
        return;
    }

    heartbeat_in_flight_ = true;
    try {
        auto state_value = shared_from_this();
        asio::co_spawn(loop_.executor(), ruvia::as_awaitable(heartbeat_owned(std::move(state_value))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        finish_heartbeat();
        close_on_worker(abort_reason_type::closing);
    }
}

task<void> websocket_client_state::heartbeat_owned(std::shared_ptr<websocket_client_state> state_value) {
    try {
        // A due ping waits for an in-progress application write on the same
        // signal as every other writer. The write may have refreshed activity or
        // the connection may have left the open phase meanwhile, so liveness is
        // evaluated again once the write lane is idle.
        co_await state_value->wait_for_write_idle();
        if (state_value->phase_.load(std::memory_order_acquire) == phase_type::open &&
            state_value->heartbeat_ping_due()) {
            write_guard_type write_guard(*state_value, write_phase_type::heartbeat);
            const auto challenge = ++state_value->heartbeat_sequence_;
            state_value->liveness_state_ = websocket_sending_ping(challenge);
            const auto payload_value = websocket_heartbeat_payload(challenge);
            const auto submitted = state_value->require_protocol().submit_frame(websocket_opcode::ping,
                std::string_view(payload_value.data(), payload_value.size()));
            if (submitted != websocket_frame_submit_status::accepted) {
                throw websocket_client_error(websocket_client_error::code_type::protocol_error,
                    "failed to submit WebSocket client heartbeat");
            }
            co_await state_value->flush_output();
            const auto ping_sent_at_ms = websocket_steady_now_ms();
            if (state_value->phase_.load(std::memory_order_acquire) == phase_type::open &&
                std::holds_alternative<websocket_sending_ping>(state_value->liveness_state_)) {
                state_value->liveness_state_ = websocket_awaiting_pong(ping_sent_at_ms, challenge);
                state_value->arm_heartbeat_timer(*state_value->config_.heartbeat_.pong_timeout_);
            }
        }
    } catch (...) {
        if (state_value->phase_.load(std::memory_order_acquire) != phase_type::closed) {
            state_value->close_on_worker(abort_reason_type::closing);
        }
    }
    state_value->finish_heartbeat();
}

void websocket_client_state::finish_heartbeat() noexcept {
    if (!heartbeat_in_flight_) {
        std::terminate();
    }
    heartbeat_in_flight_ = false;
    close_state_.notify_progress();
}

}  // namespace ruvia::detail
