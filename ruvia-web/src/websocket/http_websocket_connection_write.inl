#pragma once

namespace ruvia::detail {

template <typename stream_type>
task<void> websocket_connection<stream_type>::write(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    require_current_worker();
    return write_owned(opcode, payload_value, write_operation_lease_type(*this), compress);
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::write_owned(websocket_opcode opcode, std::string_view payload_value, write_operation_lease_type write_lease, bool compress) {
    require_current_worker();
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        co_await write_exclusive(opcode, payload_value, compress);
    }
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::close(::ruvia::websocket_close_options options) {
    require_current_worker();
    if (read_phase_ == read_phase_type::reserved) {
        throw std::logic_error("websocket close cannot overlap a pending read");
    }
    return close_owned(options, write_operation_lease_type(*this));
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::close_owned(::ruvia::websocket_close_options options, write_operation_lease_type write_lease) {
    require_current_worker();
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        co_await wait_for_write_idle();
        const auto reason = options.reason_.view();
        bool flush_output = false;
        {
            write_guard_type write_guard(*this, write_phase_type::application);
            switch (protocol_.submit_close(options.code_, reason)) {
                case websocket_close_submit_status::accepted:
                    // The deadline covers the Close write too: a peer that stops
                    // reading must not hold the flush below open past the timeout.
                    liveness_state_ = websocket_awaiting_peer_close(websocket_steady_now_ms());
                    flush_output = true;
                    break;
                case websocket_close_submit_status::already_closing:
                    flush_output = true;
                    break;
                case websocket_close_submit_status::closed:
                    break;
                case websocket_close_submit_status::invalid_code:
                    throw std::invalid_argument("invalid websocket close code");
                case websocket_close_submit_status::invalid_reason:
                    throw std::invalid_argument("invalid websocket close reason");
                case websocket_close_submit_status::reason_too_large:
                    throw std::invalid_argument("websocket close reason is too large");
            }
            if (flush_output) {
                co_await flush_protocol_output_now();
            }
        }

        // RFC 6455: sending Close starts, but does not complete, the handshake.
        // Keep parsing transport input until the peer Close arrives (or EOF/timeout
        // aborts this transport). The core suppresses application messages in this
        // phase while still validating frames and handling control traffic. The
        // Close may be this call's or the one a draining server already sent.
        if (protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_close) {
            if (read_phase_ == read_phase_type::active) {
                while (read_phase_ == read_phase_type::active) {
                    co_await reader_done_signal_.wait();
                }
            } else {
                (void)co_await read();
            }
        }
    }
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::detach_and_drain_writes() {
    require_current_worker();
    periodic_check_.reset();
    // Teardown first cancels the transport operation that owns a suspended
    // write, then joins that write before the connection storage disappears.
    // Application writes can outlive the handler through the public facade just
    // as heartbeat writes can outlive the scanner callback, so both phases are
    // part of the same structured drain. Force a transport abort when active
    // I/O remains even if the protocol already committed its normal close.
    const bool has_active_io = read_phase_ == read_phase_type::active || write_phase_ != write_phase_type::idle;
    abort_transport(has_active_io);
    while (has_operations_to_drain()) {
        if (read_phase_ != read_phase_type::idle) {
            co_await reader_done_signal_.wait();
        } else {
            co_await background_write_signal_.wait();
        }
    }
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::wait_for_write_idle() {
    while (write_phase_ != write_phase_type::idle) {
        co_await background_write_signal_.wait();
    }
}

template <typename stream_type>
void websocket_connection<stream_type>::notify_write_idle() noexcept {
    background_write_signal_.notify();
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::write_exclusive(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    co_await wait_for_write_idle();
    write_guard_type write_guard(*this, write_phase_type::application);
    co_await write_frame_now(opcode, payload_value, compress);
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::write_frame_now(
    websocket_opcode opcode, std::string_view payload_value, bool compress) {
    switch (protocol_.submit_frame(opcode, payload_value, compress)) {
        case websocket_frame_submit_status::accepted:
            break;
        case websocket_frame_submit_status::not_open:
            co_return;
        case websocket_frame_submit_status::invalid_opcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case websocket_frame_submit_status::message_too_large:
            throw std::invalid_argument("websocket message is too large");
        case websocket_frame_submit_status::invalid_text_payload:
            throw std::invalid_argument("websocket text payload is not valid UTF-8");
        case websocket_frame_submit_status::control_frame_too_large:
            throw std::invalid_argument("websocket control frame is too large");
    }
    co_await flush_protocol_output_now();
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::flush_protocol_output_exclusive() {
    co_await wait_for_write_idle();
    write_guard_type write_guard(*this, write_phase_type::application);
    co_await flush_protocol_output_now();
}

template <typename stream_type>
task<void> websocket_connection<stream_type>::flush_protocol_output_now() {
    for (;;) {
        const auto plan = protocol_.output_plan();
        const auto disposition = plan.disposition();
        if (plan.bytes().empty() && disposition == websocket_transport_disposition::keep_open) {
            co_return;
        }
        const auto ec = co_await write_bytes(plan.bytes());
        if (ec) {
            close_stream();
            (void)protocol_.abort();
            throw inbound_transport_error(ec, "failed to write websocket bytes");
        }
        if (protocol_.consume_output(plan.bytes().size()) != websocket_output_consume_status::drained) {
            std::terminate();
        }
        scanner_entry_.touch();
        if (disposition == websocket_transport_disposition::end_transport) {
            protocol_.commit_transport_end();
            co_return;
        }
    }
}

template <typename stream_type>
void websocket_connection<stream_type>::abort_transport(bool force_transport) noexcept {
    liveness_state_ = websocket_liveness_idle{};
    const auto disposition = protocol_.abort();
    if (force_transport || disposition == websocket_abort_disposition::abort_transport) {
        close_stream();
        notify_write_idle();
    }
}

template <typename stream_type>
task<std::error_code> websocket_connection<stream_type>::write_bytes(std::string_view bytes_value) {
    if (bytes_value.empty()) {
        co_return std::error_code{};
    }
    const auto buffer = asio::buffer(bytes_value.data(), bytes_value.size());
    const auto write_completion = co_await ruvia::async_asio([this, buffer](auto handler) mutable {
        // Partial progress is transport activity: a slow reader that keeps
        // draining a large write must not look like a stalled peer to the
        // heartbeat.
        asio::async_write(stream_, buffer, [this](const std::error_code& ec, std::size_t bytes_transferred) noexcept {
                if (bytes_transferred != 0) {
                    scanner_entry_.touch();
                }
                return asio::transfer_all()(ec, bytes_transferred); }, std::move(handler));
    });
    co_return write_completion.error_code();
}

template <typename stream_type>
void websocket_connection<stream_type>::close_stream() noexcept {
    if constexpr (requires(stream_type& value) { value.next_layer(); }) {
        ruvia::close_socket(stream_.next_layer());
    } else {
        ruvia::close_socket(stream_);
    }
}

}  // namespace ruvia::detail
