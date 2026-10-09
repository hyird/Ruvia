#pragma once

#include "ruvia/http/websocket_protocol.h"

#include "server/inbound_buffer_resource.h"

namespace ruvia::detail {

template <typename transport_type>
task<std::optional<websocket_message>> websocket_connection<transport_type>::read() {
    require_current_worker();
    return read_owned(read_guard_type(*this));
}

template <typename transport_type>
task<std::optional<websocket_message>> websocket_connection<transport_type>::read_owned(read_guard_type read_guard_value) {
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
                    const auto read_result_value = co_await transport_.read_more(buffer_);
                    if (const auto* failure = read_result_value.failure()) {
                        transport_.abort();
                        (void)protocol_.abort();
                        throw std::system_error(failure->error_code(), "failed to read websocket bytes");
                    }
                    if (read_result_value.end() != nullptr) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and asks the
                        // transport adapter to finish only its own direction/stream.
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

}  // namespace ruvia::detail
