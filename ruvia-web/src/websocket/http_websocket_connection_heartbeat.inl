#pragma once

#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/task.h"
#include "ruvia/http/websocket_protocol.h"

#include "util/steady_clock.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

template <typename stream_type>
bool server_websocket_connection<stream_type>::pong_observable() const noexcept {
    return !write_lane_.write_in_progress() && read_phase_ == read_phase_type::active;
}

// The Pong wait is measured only while the reader can poll it. Restarting the
// window when that becomes true (or at every tick while it is false) keeps a
// Pong that was already buffered during a write from being timed out before
// the reader's next poll gets to it.
template <typename stream_type>
void server_websocket_connection<stream_type>::restart_pong_window(std::int64_t now) noexcept {
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_)) {
        liveness_state_ = websocket_awaiting_pong(now, pong->challenge());
    }
}

template <typename stream_type>
void server_websocket_connection<stream_type>::resume_pong_window_on_read_start() noexcept {
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_);
        pong != nullptr && !write_lane_.write_in_progress() &&
        lifecycle_options_.heartbeat_.pong_timeout_.has_value()) {
        const auto now = steady_now_ms();
        if (now - pong->sent_at_ms() >= lifecycle_options_.heartbeat_.pong_timeout_->count()) {
            liveness_state_ = websocket_awaiting_pong(now, pong->challenge());
        }
    }
}

template <typename stream_type>
void server_websocket_connection<stream_type>::finish_write(bool restart_pong) noexcept {
    write_lane_.release();
    if (restart_pong && std::holds_alternative<websocket_awaiting_pong>(liveness_state_)) {
        restart_pong_window(steady_now_ms());
    }
    write_lane_.notify();
}

template <typename stream_type>
void server_websocket_connection<stream_type>::heartbeat_tick(std::int64_t now) noexcept {
    const bool observable = pong_observable();
    if (!observable) {
        restart_pong_window(now);
    }
    switch (evaluate_websocket_liveness(lifecycle_options_, protocol_.liveness_mode(), liveness_state_,
        write_lane_.write_in_progress(), observable, scanner_entry_.last_active_ms(), now)) {
        case websocket_liveness_decision::idle:
            return;
        case websocket_liveness_decision::abort_transport:
            // A heartbeat or close timeout closes the connection.
            abort_transport();
            return;
        case websocket_liveness_decision::send_ping:
            break;
    }
    if (write_lane_.reader_waiting()) {
        // Let an already blocked reader poll its buffered control frames
        // before a scanner heartbeat claims the next message boundary.
        return;
    }

    liveness_state_ = websocket_sending_ping(++heartbeat_sequence_);
    write_lane_.begin();
    try {
        background_tasks_.spawn(write_heartbeat_ping());
    } catch (...) {
        finish_write();
        fail_background(std::current_exception());
        return;
    }
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::write_heartbeat_ping() {
    write_guard_type write_guard(*this, write_claim_type::adopt);
    try {
        const auto* sending = std::get_if<websocket_sending_ping>(&liveness_state_);
        if (sending == nullptr) {
            co_return;
        }
        const auto challenge = sending->challenge();
        const auto payload_value = websocket_heartbeat_payload(challenge);
        if (accept_submitted_frame(protocol_.submit_frame(
                websocket_opcode::ping, std::string_view(payload_value.data(), payload_value.size())))) {
            co_await flush_protocol_output_now();
        }
        // Ordinary I/O shares the scanner activity timestamp and may update it
        // while this coroutine is suspended. The Pong deadline belongs to this
        // completed heartbeat write, so use its own timestamp.
        const auto ping_sent_at_ms = steady_now_ms();
        if (protocol_.liveness_mode() == websocket_liveness_mode::open &&
            std::holds_alternative<websocket_sending_ping>(liveness_state_)) {
            liveness_state_ = websocket_awaiting_pong(ping_sent_at_ms, challenge);
        }
    } catch (...) {
        fail_background(std::current_exception());
    }
}

template <typename stream_type>
void server_websocket_connection<stream_type>::fail_background(std::exception_ptr failure) noexcept {
    abort_transport();
    if (background_failure_ == nullptr) {
        background_failure_ = std::move(failure);
    }
}

}  // namespace ruvia::detail
