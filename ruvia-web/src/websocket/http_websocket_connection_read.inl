#pragma once

#include <algorithm>

#include "ruvia/http/websocket_protocol.h"

#include "server/inbound_buffer_resource.h"
#include "server/inbound_transport_error.h"

namespace ruvia::detail {

template <typename stream_type>
task<std::optional<websocket_message>> websocket_connection<stream_type>::read() {
    require_current_worker();
    return read_owned(read_guard_type(*this));
}

template <typename stream_type>
task<std::optional<websocket_message>> websocket_connection<stream_type>::read_owned(read_guard_type read_guard_value) {
    require_current_worker();
    {
        read_guard_type active_read(std::move(read_guard_value));
        active_read.start();
        static_cast<void>(active_read);
        try {
            for (;;) {
                // poll() can queue an automatic Pong/Close. Keep that mutation mutually
                // exclusive with an in-flight application/heartbeat write so the core's
                // pending-output storage cannot reallocate under async I/O.
                co_await wait_for_write_idle();
                const auto event = protocol_.poll();
                if (!event.has_value()) {
                    const auto read_result_value = co_await read_more();
                    if (const auto* failure = read_result_value.failure()) {
                        close_stream();
                        (void)protocol_.abort();
                        throw inbound_transport_error(failure->error_code(), "failed to read websocket bytes");
                    }
                    if (read_result_value.end() != nullptr) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and plans the
                        // transport end.
                        protocol_.notify_transport_eof();
                        co_await flush_protocol_output_exclusive();
                        co_return std::nullopt;
                    }
                    if (read_result_value.data() == nullptr) {
                        throw std::logic_error("unexpected WebSocket transport read result");
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
task<http_stream_read_result> websocket_connection<stream_type>::read_more() {
    // Fill the spare capacity the buffer already owns, within bounds: growth
    // past it follows the string's geometric policy, so a large frame needs
    // far fewer reads than one per 4 KiB, while the cap keeps the per-read
    // resize fill small when a large buffer is retained.
    constexpr std::size_t min_read_bytes = 4096;
    constexpr std::size_t max_read_bytes = 64 * 1024;
    const auto old_size = buffer_.size();
    const auto read_bytes = std::clamp(buffer_.capacity() - old_size, min_read_bytes, max_read_bytes);
    http_resize_pmr_string_for_overwrite(buffer_, old_size + read_bytes);
    auto read_completion = co_await ruvia::async_asio<std::size_t>([this, old_size](auto handler) mutable {
        stream_.async_read_some(asio::buffer(buffer_.data() + old_size, buffer_.size() - old_size), std::move(handler));
    });
    const auto ec = read_completion.error_code();
    const auto bytes_read = read_completion.result();
    // An orderly peer close (TCP FIN or TLS close_notify) and a TLS peer
    // that dropped TCP without close_notify both end the byte stream; the
    // websocket core maps a missing Close frame to an abnormal closure.
    if (is_stream_read_end(ec) || (!ec && bytes_read == 0)) {
        buffer_.resize(old_size);
        co_return http_stream_read_result::make_end();
    }
    if (ec) {
        buffer_.resize(old_size);
        co_return http_stream_read_result::make_failure(ec);
    }
    buffer_.resize(old_size + bytes_read);
    co_return http_stream_read_result::make_data();
}

}  // namespace ruvia::detail
