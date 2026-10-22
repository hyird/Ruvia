#pragma once

#include <array>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

#include "server/connection_scanner.h"
#include "server/inbound_transport_error.h"
#include "util/steady_clock.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

struct websocket_write_progress final {
    connection_scanner::entry_type* entry_;

    [[nodiscard]] std::size_t operator()(
        const std::error_code& ec, std::size_t bytes_transferred) const noexcept {
        if (bytes_transferred != 0) {
            entry_->touch();
        }
        return asio::transfer_all()(ec, bytes_transferred);
    }
};

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::write(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    require_current_worker();
    return write_owned(opcode, payload_value, write_operation_lease_type(*this), compress);
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::write_owned(websocket_opcode opcode, std::string_view payload_value, write_operation_lease_type write_lease, bool compress) {
    require_current_worker();
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (write_phase_ != write_phase_type::idle) {
            co_await wait_for_write_idle();
        }
        write_guard_type write_guard(*this, write_phase_type::application);
        // The caller keeps the payload alive until this write completes, so an
        // uncompressed frame is gathered from it instead of being copied.
        borrowed_frame_guard_type borrowed_frame(*this, opcode, payload_value, compress);
        if (borrowed_frame.submitted()) {
            co_await flush_protocol_output_now();
            borrowed_frame.release();
        }
    }
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::close(::ruvia::websocket_close_options options) {
    require_current_worker();
    if (read_phase_ == read_phase_type::reserved) {
        throw std::logic_error("websocket close cannot overlap a pending read");
    }
    return close_owned(options, write_operation_lease_type(*this));
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::close_owned(::ruvia::websocket_close_options options, write_operation_lease_type write_lease) {
    require_current_worker();
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (write_phase_ != write_phase_type::idle) {
            co_await wait_for_write_idle();
        }
        const auto reason = options.reason_.view();
        bool flush_output = false;
        {
            write_guard_type write_guard(*this, write_phase_type::application);
            switch (protocol_.submit_close(options.code_, reason)) {
                case websocket_close_submit_status::accepted:
                    // The deadline covers the Close write too: a peer that stops
                    // reading must not hold the flush below open past the timeout.
                    liveness_state_ = websocket_awaiting_peer_close(steady_now_ms());
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
task<void> server_websocket_connection<stream_type>::detach_and_drain_writes() {
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
task<void> server_websocket_connection<stream_type>::wait_for_write_idle() {
    while (write_phase_ != write_phase_type::idle) {
        co_await background_write_signal_.wait();
    }
}

template <typename stream_type>
void server_websocket_connection<stream_type>::notify_write_idle() noexcept {
    background_write_signal_.notify();
}

template <typename stream_type>
bool server_websocket_connection<stream_type>::accept_submitted_frame(websocket_frame_submit_status status) {
    switch (status) {
        case websocket_frame_submit_status::accepted:
            return true;
        case websocket_frame_submit_status::not_open:
            return false;
        case websocket_frame_submit_status::invalid_opcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case websocket_frame_submit_status::message_too_large:
            throw std::invalid_argument("websocket message is too large");
        case websocket_frame_submit_status::invalid_text_payload:
            throw std::invalid_argument("websocket text payload is not valid UTF-8");
        case websocket_frame_submit_status::control_frame_too_large:
            throw std::invalid_argument("websocket control frame is too large");
    }
    throw std::logic_error("unexpected websocket frame submit status");
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::flush_protocol_output_exclusive() {
    if (write_phase_ != write_phase_type::idle) {
        co_await wait_for_write_idle();
    }
    write_guard_type write_guard(*this, write_phase_type::application);
    co_await flush_protocol_output_now();
}

template <typename stream_type>
auto server_websocket_connection<stream_type>::write_bytes(const websocket_output_plan& plan) {
    if constexpr (!native_stream) {
        return write_adapted(plan);
    } else {
        // One gathered write: pending protocol output, then a borrowed payload.
        const std::array<asio::const_buffer, 2> buffers{
            asio::buffer(plan.bytes().data(), plan.bytes().size()),
            asio::buffer(plan.borrowed_bytes().data(), plan.borrowed_bytes().size())};
        return ruvia::async_asio([this, buffers](auto handler) mutable {
            // Partial progress refreshes heartbeat transport activity.
            asio::async_write(
                stream_, buffers, websocket_write_progress{&scanner_entry_}, std::move(handler));
        });
    }
}

template <typename stream_type>
task<std::error_code> server_websocket_connection<stream_type>::write_adapted(const websocket_output_plan& plan) {
    if (const auto error = co_await stream_.write_bytes(plan.bytes(), websocket_transport_disposition::keep_open)) {
        co_return error;
    }
    co_return co_await stream_.write_bytes(plan.borrowed_bytes(), plan.disposition());
}

template <typename stream_type>
task<void> server_websocket_connection<stream_type>::flush_protocol_output_now() {
    for (;;) {
        const auto plan = protocol_.output_plan();
        const auto disposition = plan.disposition();
        const auto size = plan.size();
        if (size == 0 && disposition == websocket_transport_disposition::keep_open) {
            co_return;
        }
        if (size != 0 || (!native_stream && disposition == websocket_transport_disposition::end_transport)) {
            const auto write_completion = co_await write_bytes(plan);
            std::error_code ec;
            if constexpr (native_stream) {
                ec = write_completion.error_code();
            } else {
                ec = write_completion;
            }
            if (ec) {
                close_stream();
                (void)protocol_.abort();
                throw inbound_transport_error(ec, "failed to write websocket bytes");
            }
        }
        if (protocol_.consume_output(size) != websocket_output_consume_status::drained) {
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
void server_websocket_connection<stream_type>::abort_transport(bool force_transport) noexcept {
    liveness_state_ = websocket_liveness_idle{};
    const auto disposition = protocol_.abort();
    if (force_transport || disposition == websocket_abort_disposition::abort_transport) {
        close_stream();
        notify_write_idle();
    }
}

template <typename stream_type>
void server_websocket_connection<stream_type>::close_stream() noexcept {
    if constexpr (!native_stream) {
        stream_.abort();
    } else if constexpr (requires(stream_type& value) { value.next_layer(); }) {
        ruvia::close_socket(stream_.lowest_layer());
    } else {
        ruvia::close_socket(stream_);
    }
}

}  // namespace ruvia::detail
