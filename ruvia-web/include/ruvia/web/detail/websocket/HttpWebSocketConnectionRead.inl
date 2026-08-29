#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::read() {
    return readOwned(ReadGuard(*this));
}

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::readOwned(ReadGuard readGuard) {
    {
        ReadGuard activeRead(std::move(readGuard));
        activeRead.start();
        static_cast<void>(activeRead);
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
                co_return WebSocketMessageAccess::make(message->opcode(), message->payload());
            }
            if (event->ping() != nullptr) {
                co_await flushProtocolOutputExclusive();
                continue;
            }
            if (event->pong() != nullptr) {
                if (std::holds_alternative<WebSocketSendingPing>(livenessState_) || std::holds_alternative<WebSocketAwaitingPong>(livenessState_)) {
                    livenessState_ = WebSocketLivenessIdle{};
                }
                continue;
            }
            if (event->close() != nullptr || event->protocolError() != nullptr || event->transportEnd() != nullptr) {
                // These observations terminate the application read side. WsOutputPlan
                // remains the sole authority for flushing Close bytes and mapping
                // orderly transport completion.
                livenessState_ = WebSocketLivenessIdle{};
                co_await flushProtocolOutputExclusive();
                co_return std::nullopt;
            }
            throw std::logic_error("unexpected WebSocket protocol event");
        }
    }
}

}  // namespace ruvia::detail
