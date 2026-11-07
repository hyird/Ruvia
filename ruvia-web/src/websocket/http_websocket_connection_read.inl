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
                std::optional<websocket_event> event;
                {
                    // RFC 6455 sections 5.5.1/5.5.2: poll() may queue an
                    // automatic Close/Pong. Reserve the next message boundary
                    // and retain the lane through that control output flush.
                    if (write_lane_.write_in_progress()) {
                        co_await wait_for_write_idle(write_wait_type::reader);
                    }
                    write_guard_type write_guard(*this, write_claim_type::acquire, false);
                    event = protocol_.next_event();
                    if (event.has_value()) {
                        if (event->ping() != nullptr) {
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        } else if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                            if (const auto* close_event = event->close()) {
                                record_peer_close(*close_event);
                            }
                            liveness_state_ = websocket_liveness_idle{};
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        }
                    }
                }
                if (!event.has_value()) {
                    if (!(co_await read_more())) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and plans the
                        // transport end.
                        if (write_lane_.write_in_progress()) {
                            co_await wait_for_write_idle(write_wait_type::reader);
                        }
                        write_guard_type write_guard(*this);
                        protocol_.notify_transport_eof();
                        co_await flush_protocol_output_now();
                        close_stream();
                        co_return std::nullopt;
                    }
                    scanner_entry_.touch();
                    continue;
                }

                if (const auto* message = event->message()) {
                    co_return *message;
                }
                if (event->ping() != nullptr) {
                    continue;
                }
                if (event->pong() != nullptr) {
                    if (websocket_heartbeat_pong_matches(liveness_state_, event->pong()->payload())) {
                        liveness_state_ = websocket_liveness_idle{};
                        scanner_entry_.touch();
                    }
                    continue;
                }
                if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                    // The poll's exclusive lane already flushed the protocol's
                    // Close/end-transport plan before this read completes.
                    co_return std::nullopt;
                }
                throw std::logic_error("unexpected WebSocket protocol event");
            }
        } catch (...) {
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
            abort_transport(true);
            throw;
        }
    } else {
        constexpr std::size_t min_read_bytes = 4096;
        constexpr std::size_t max_read_bytes = 64 * 1024;
        const bool between_frames = buffer_.empty();
        std::span<char> read_space;
        if (between_frames) {
            // Preserve the native socket's small idle read allocation and
            // retained storage for steady mid-size messages.
            if (buffer_.capacity() > websocket_retained_buffer_bytes) {
                buffer_.shrink_to(websocket_retained_buffer_bytes);
            }
            const auto min_idle_bytes = small_frames_ ? websocket_idle_read_bytes : min_read_bytes;
            if (buffer_.capacity() < min_idle_bytes) {
                read_space = buffer_.prepare(min_idle_bytes);
            } else {
                read_space = buffer_.prepare_spare();
                if (small_frames_ && read_space.size() > websocket_idle_read_bytes) {
                    read_space = read_space.first(websocket_idle_read_bytes);
                } else if (read_space.size() > max_read_bytes) {
                    read_space = read_space.first(max_read_bytes);
                }
            }
        } else {
            // Reclaim any consumed prefix first; grow geometrically within a
            // frame only when spare storage is smaller than needed.
            const auto needed = std::clamp(protocol_.needed_input_bytes(), min_read_bytes, max_read_bytes);
            if (buffer_.capacity() - buffer_.size() >= needed) {
                read_space = buffer_.prepare_spare();
                if (read_space.size() > max_read_bytes) {
                    read_space = read_space.first(max_read_bytes);
                }
            } else {
                read_space = buffer_.prepare(needed);
            }
        }
        auto read_completion = co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
            stream_.async_read_some(asio::buffer(read_space), std::move(handler));
        });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        // The protocol maps EOF without a Close frame to abnormal closure.
        if (is_stream_read_end(ec) || (!ec && bytes_read == 0)) {
            co_return false;
        }
        if (ec) {
            abort_transport(true);
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
