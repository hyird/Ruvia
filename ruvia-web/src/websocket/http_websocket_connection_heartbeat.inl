#pragma once

namespace ruvia::detail {

template <typename stream_type>
void websocket_connection<stream_type>::finish_write(write_phase_type phase) noexcept {
    if (write_phase_ != phase) {
        std::terminate();
    }
    write_phase_ = write_phase_type::idle;
    background_write_signal_.notify();
}

template <typename stream_type>
void websocket_connection<stream_type>::heartbeat_tick(std::int64_t now) noexcept {
    switch (evaluate_websocket_liveness(lifecycle_options_, protocol_.liveness_mode(), liveness_state_,
        write_phase_ != write_phase_type::idle, scanner_entry_.last_active_ms(), now)) {
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
        abort_transport();
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
        co_await write_frame_now(websocket_opcode::ping, std::string_view(payload_value.data(), payload_value.size()));
        // Ordinary I/O shares the scanner activity timestamp and may update it
        // while this coroutine is suspended. The Pong deadline belongs to this
        // completed heartbeat write, so use its own timestamp.
        const auto ping_sent_at_ms = websocket_steady_now_ms();
        if (protocol_.liveness_mode() == websocket_liveness_mode::open &&
            std::holds_alternative<websocket_sending_ping>(liveness_state_)) {
            liveness_state_ = websocket_awaiting_pong(ping_sent_at_ms, challenge);
        }
    } catch (...) {
        abort_transport();
    }
}

}  // namespace ruvia::detail
