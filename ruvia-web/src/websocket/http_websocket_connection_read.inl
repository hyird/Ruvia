#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

#include "server/inbound_buffer_resource.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_stream_end.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::optional<websocket_message>> server_websocket_connection<stream_type>::read() {
    require_current_worker();
    return read_owned(read_guard_type(*this));
}

template <typename stream_type>
task<std::optional<websocket_message>> server_websocket_connection<stream_type>::read_owned(read_guard_type read_guard_value) {
    require_current_worker();
    {
        read_guard_type active_read(std::move(read_guard_value));
        active_read.start();
        try {
            for (;;) {
                // poll() can queue an automatic Pong/Close. Keep that mutation mutually
                // exclusive with an in-flight application/heartbeat write so the core's
                // pending-output storage cannot reallocate under async I/O.
                if (write_phase_ != write_phase_type::idle) {
                    co_await wait_for_write_idle();
                }
                const auto event = protocol_.next_event();
                if (!event.has_value()) {
                    if (!(co_await read_more())) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and plans the
                        // transport end.
                        protocol_.notify_transport_eof();
                        co_await flush_protocol_output_exclusive();
                        co_return std::nullopt;
                    }
                    scanner_entry_.touch();
                    continue;
                }

                if (const auto* message = event->message()) {
                    co_return websocket_message::borrow(message->opcode(), message->payload());
                }
                if (event->ping() != nullptr) {
                    co_await flush_protocol_output_exclusive();
                    continue;
                }
                if (event->pong() != nullptr) {
                    if (websocket_heartbeat_pong_matches(liveness_state_, event->pong()->payload())) {
                        liveness_state_ = websocket_liveness_idle{};
                    }
                    continue;
                }
                if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                    // These observations terminate the application read side. websocket_output_plan
                    // remains the sole authority for flushing Close bytes and mapping
                    // orderly transport completion.
                    liveness_state_ = websocket_liveness_idle{};
                    co_await flush_protocol_output_exclusive();
                    co_return std::nullopt;
                }
                throw std::logic_error("unexpected WebSocket protocol event");
            }
        } catch (const inbound_buffer_limit_error&) {
            abort_transport(true);
            throw;
        }
    }
}

template <typename stream_type>
task<bool> server_websocket_connection<stream_type>::read_more() {
    if constexpr (!native_stream) {
        try {
            co_return co_await stream_.read_more(buffer_);
        } catch (const inbound_transport_error&) {
            close_stream();
            (void)protocol_.abort();
            throw;
        }
    } else {
        constexpr std::size_t min_read_bytes = 4096;
        constexpr std::size_t max_read_bytes = 64 * 1024;
        const bool between_frames = buffer_.empty();
        std::size_t read_bytes = 0;
        if (between_frames) {
            // Preserve the native socket's small idle read allocation and
            // retained storage for steady mid-size messages.
            read_bytes = small_frames_ ? websocket_idle_read_bytes : (std::max)(buffer_.capacity(), min_read_bytes);
            buffer_.shrink_to(read_bytes);
        } else {
            // Grow geometrically within a frame; read space is uninitialized.
            read_bytes = std::clamp((std::max)(buffer_.capacity() - buffer_.size(), protocol_.needed_input_bytes()),
                min_read_bytes, max_read_bytes);
        }
        const auto read_space = buffer_.prepare(read_bytes);
        auto read_completion = co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
            stream_.async_read_some(asio::buffer(read_space.data(), read_space.size()), std::move(handler));
        });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        // The protocol maps EOF without a Close frame to abnormal closure.
        if (is_stream_read_end(ec) || (!ec && bytes_read == 0)) {
            co_return false;
        }
        if (ec) {
            close_stream();
            (void)protocol_.abort();
            throw inbound_transport_error(ec, "failed to read websocket bytes");
        }
        buffer_.commit(bytes_read);
        if (between_frames) {
            small_frames_ = bytes_read < websocket_idle_read_bytes;
        }
        co_return true;
    }
}

}  // namespace ruvia::detail
