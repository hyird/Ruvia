#pragma once

namespace ruvia::detail {

template <typename stream_type>
bool websocket_connection<stream_type>::pong_observable() const noexcept {
    return write_phase_ == write_phase_type::idle && read_phase_ == read_phase_type::active;
}

// The Pong wait is measured only while the reader can poll it. Restarting the
// window when that becomes true (or at every tick while it is false) keeps a
// Pong that was already buffered during a write from being timed out before
// the reader's next poll gets to it.
template <typename stream_type>
void websocket_connection<stream_type>::restart_pong_window(std::int64_t now) noexcept {
    if (const auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_state_)) {
        liveness_state_ = websocket_awaiting_pong(now, pong->challenge());
    }
}

template <typename stream_type>
void websocket_connection<stream_type>::finish_write(write_phase_type phase) noexcept {
    if (write_phase_ != phase) {
        std::terminate();
    }
    write_phase_ = write_phase_type::idle;
    restart_pong_window(steady_now_ms());
    if (going_away_pending_) {
        start_going_away();
    }
    background_write_signal_.notify();
}

template <typename stream_type>
void websocket_connection<stream_type>::start_going_away() noexcept {
    if (protocol_.liveness_mode() != websocket_liveness_mode::open) {
        // A close handshake (or a terminal failure) is already under way.
        going_away_pending_ = false;
        return;
    }
    if (write_phase_ != write_phase_type::idle) {
        // The lane owner's finish_write() starts the close.
        going_away_pending_ = true;
        return;
    }
    going_away_pending_ = false;
    write_phase_ = write_phase_type::going_away;
    try {
        asio::co_spawn(stream_.get_executor(), ruvia::as_awaitable(write_going_away_close()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        finish_write(write_phase_type::going_away);
        fail_background(std::current_exception());
    }
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::write_going_away_close() {
    write_guard_type write_guard(*this, write_phase_type::going_away, write_claim_type::adopt);
    try {
        // The session may have closed or failed before this coroutine ran.
        if (protocol_.submit_close(1001, {}) != websocket_close_submit_status::accepted) {
            co_return;
        }
        // Same deadline as a locally initiated close(); it covers this write.
        liveness_state_ = websocket_awaiting_peer_close(steady_now_ms());
        co_await flush_protocol_output_now();
    } catch (...) {
        fail_background(std::current_exception());
    }
}

template <typename stream_type>
void websocket_connection<stream_type>::heartbeat_tick(std::int64_t now) noexcept {
    const bool observable = pong_observable();
    if (!observable) {
        restart_pong_window(now);
    }
    switch (evaluate_websocket_liveness(lifecycle_options_, protocol_.liveness_mode(), liveness_state_,
        write_phase_ != write_phase_type::idle, observable, scanner_entry_.last_active_ms(), now)) {
        case websocket_liveness_decision::idle:
            return;
        case websocket_liveness_decision::abort_transport:
            // A heartbeat or close timeout closes the connection.
            abort_transport();
            return;
        case websocket_liveness_decision::send_ping:
            break;
    }

    liveness_state_ = websocket_sending_ping(++heartbeat_sequence_);
    write_phase_ = write_phase_type::heartbeat;
    try {
        asio::co_spawn(stream_.get_executor(), ruvia::as_awaitable(write_heartbeat_ping()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        finish_write(write_phase_type::heartbeat);
        fail_background(std::current_exception());
        return;
    }
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::write_heartbeat_ping() {
    write_guard_type write_guard(*this, write_phase_type::heartbeat, write_claim_type::adopt);
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
void websocket_connection<stream_type>::fail_background(std::exception_ptr failure) noexcept {
    abort_transport();
    if (background_failure_ == nullptr) {
        background_failure_ = std::move(failure);
    }
}

}  // namespace ruvia::detail
