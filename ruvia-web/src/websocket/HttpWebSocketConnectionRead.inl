#pragma once

#include "ruvia/http/WebSocketProtocol.h"

#include "server/inbound_buffer_resource.h"

namespace ruvia::detail {

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::read() {
    requireCurrentWorker();
    return readOwned(ReadGuard(*this));
}

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::readOwned(ReadGuard readGuard) {
    requireCurrentWorker();
    {
        ReadGuard activeRead(std::move(readGuard));
        activeRead.start();
        static_cast<void>(activeRead);
        try {
            for (;;) {
                // poll() can queue an automatic Pong/Close. Keep that mutation mutually
                // exclusive with an in-flight application/heartbeat write so the core's
                // pending-output storage cannot reallocate under async I/O.
                co_await waitForWriteIdle();
                const auto event = protocol_.poll();
                if (!event.has_value()) {
                    const auto readResult = co_await transport_.readMore(buffer_);
                    if (const auto* failure = readResult.failure()) {
                        transport_.abort();
                        (void)protocol_.abort();
                        throw std::system_error(failure->errorCode(), "failed to read websocket bytes");
                    }
                    if (readResult.end() != nullptr) {
                        // EOF is an abnormal WebSocket close when no peer Close was
                        // received. The core discards unsent WS output and asks the
                        // transport adapter to finish only its own direction/stream.
                        protocol_.notifyTransportEof();
                        co_await flushProtocolOutputExclusive();
                        co_return std::nullopt;
                    }
                    if (readResult.data() == nullptr) {
                        throw std::logic_error("unexpected WebSocket transport read result");
                    }
                    scannerEntry_.touch();
                    continue;
                }

                if (const auto* message = event->message()) {
                    co_return WebSocketMessage::borrow(message->opcode(), message->payload());
                }
                if (event->ping() != nullptr) {
                    co_await flushProtocolOutputExclusive();
                    continue;
                }
                if (event->pong() != nullptr) {
                    if (webSocketHeartbeatPongMatches(livenessState_, event->pong()->payload())) {
                        livenessState_ = WebSocketLivenessIdle{};
                    }
                    continue;
                }
                if (event->close() != nullptr || event->protocolError() != nullptr || event->transportEnd() != nullptr) {
                    // These observations terminate the application read side. WebSocketOutputPlan
                    // remains the sole authority for flushing Close bytes and mapping
                    // orderly transport completion.
                    livenessState_ = WebSocketLivenessIdle{};
                    co_await flushProtocolOutputExclusive();
                    co_return std::nullopt;
                }
                throw std::logic_error("unexpected WebSocket protocol event");
            }
        } catch (const inbound_buffer_limit_error&) {
            abortTransport(true);
            throw;
        }
    }
}

}  // namespace ruvia::detail
