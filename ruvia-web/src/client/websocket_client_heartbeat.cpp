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

void websocket_client_state::heartbeat_timer_fired() noexcept {
    if (phase_.load(std::memory_order_acquire) != phase_type::open ||
        !config_.heartbeat_.ping_interval_.has_value()) {
        return;
    }

    const auto now = websocket_steady_now_ms();
    const websocket_lifecycle_options options{
        .heartbeat_ = config_.heartbeat_, .close_handshake_timeout_ = config_.close_handshake_timeout_};
    switch (evaluate_websocket_liveness(options, require_protocol().liveness_mode(), liveness_state_,
        write_phase_ != write_phase_type::idle, last_active_ms_, now)) {
        case websocket_liveness_decision::idle:
            try {
                arm_heartbeat_timer(heartbeat_delay(now));
            } catch (...) {
                close_on_worker(abort_reason_type::closing);
            }
            return;
        case websocket_liveness_decision::abort_transport:
            close_on_worker(abort_reason_type::timeout);
            return;
        case websocket_liveness_decision::send_ping:
            break;
    }

    liveness_state_ = websocket_sending_ping(++heartbeat_sequence_);
    write_phase_ = write_phase_type::heartbeat;
    heartbeat_in_flight_ = true;
    try {
        auto state_value = shared_from_this();
        asio::co_spawn(loop_.executor(), ruvia::as_awaitable(heartbeat_owned(std::move(state_value))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        finish_heartbeat();
        finish_write(write_phase_type::heartbeat);
        close_on_worker(abort_reason_type::closing);
    }
}

task<void> websocket_client_state::heartbeat_owned(std::shared_ptr<websocket_client_state> state_value) {
    {
        write_guard_type write_guard(*state_value, write_phase_type::heartbeat, write_claim_type::adopt);
        try {
            state_value->require_open();
            const auto* sending = std::get_if<websocket_sending_ping>(&state_value->liveness_state_);
            if (sending != nullptr) {
                const auto challenge = sending->challenge();
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
