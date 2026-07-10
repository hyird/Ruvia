#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::read() {
    for (;;) {
        auto event = protocol_.nextEvent();
        if (event.kind == WsEvent::Kind::kNone) {
            // feed() may queue an automatic protocol reply. Keep it mutually
            // exclusive with an in-flight application/heartbeat write so the
            // core's pending-output storage cannot reallocate under async I/O.
            co_await waitForWriteIdle();
            (void)protocol_.feed();
            event = protocol_.nextEvent();
            if (event.kind == WsEvent::Kind::kNone) {
                if (!(co_await transport_.readMore(buffer_))) {
                    co_return std::nullopt;
                }
                scannerEntry_.touch();
                continue;
            }
        }

        switch (event.kind) {
            case WsEvent::Kind::kMessage:
                co_return WebSocketMessageAccess::make(event.opcode, event.payload);
            case WsEvent::Kind::kPing:
                co_await flushProtocolOutputExclusive(false);
                continue;
            case WsEvent::Kind::kPong:
                awaitingPong_ = false;
                continue;
            case WsEvent::Kind::kClose:
                closeSent_ = true;
                co_await flushProtocolOutputExclusive(true);
                co_return std::nullopt;
            case WsEvent::Kind::kProtocolError:
                closeSent_ = true;
                co_await flushProtocolOutputExclusive(true);
                co_return std::nullopt;
            case WsEvent::Kind::kNone:
                break;
        }
    }
}

}  // namespace ruvia::detail
