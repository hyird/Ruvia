#pragma once

namespace ruvia::detail {

template <typename Transport>
void WebSocketConnection<Transport>::completeBackgroundWrite() noexcept {
    writePhase_ = WritePhase::kIdle;
    backgroundWriteSignal_.notify();
}

template <typename Transport>
void WebSocketConnection<Transport>::heartbeatTick(std::int64_t now) noexcept {
    switch (webSocketLivenessDecision(
        lifecycleOptions_,
        protocol_.livenessMode(),
        livenessState_,
        writePhase_ != WritePhase::kIdle,
        scannerEntry_.lastActiveMs(),
        now)) {
        case WebSocketLivenessDecision::kIdle:
            return;
        case WebSocketLivenessDecision::kAbortTransport:
            // A heartbeat/close timeout belongs to this WebSocket transport.
            // For RFC 8441 that is one stream, not the multiplexed h2 socket.
            abortTransport();
            return;
        case WebSocketLivenessDecision::kSendPing:
            break;
    }

    livenessState_ = WebSocketSendingPing{};
    writePhase_ = WritePhase::kHeartbeat;
    try {
        asio::co_spawn(
            transport_.executor(),
            taskAsAwaitable(writeHeartbeatPing()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        completeBackgroundWrite();
        abortTransport();
        return;
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeHeartbeatPing() {
    try {
        co_await writeFrameNow(WebSocketOpcode::kPing, {});
        if (protocol_.livenessMode() == WsLivenessMode::kOpen &&
            std::holds_alternative<WebSocketSendingPing>(livenessState_)) {
            livenessState_ = WebSocketAwaitingPong(
                scannerEntry_.lastActiveMs());
        }
    } catch (...) {
        abortTransport();
    }
    completeBackgroundWrite();
}

}  // namespace ruvia::detail
