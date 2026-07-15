#pragma once

namespace ruvia::detail {

template <typename Transport>
void WebSocketConnection<Transport>::completeBackgroundWrite() noexcept {
    if (backgroundWriteCount_ > 0) {
        --backgroundWriteCount_;
    }
    backgroundWriteSignal_.notify();
}

template <typename Transport>
void WebSocketConnection<Transport>::heartbeatTick(std::int64_t now) noexcept {
    switch (webSocketLivenessDecision(
        lifecycleOptions_,
        protocol_.livenessMode(),
        awaitingPong_,
        writeActive_,
        scannerEntry_.lastActiveMs(),
        heartbeatPingSentMs_,
        localCloseStartedMs_,
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

    awaitingPong_ = true;
    heartbeatPingSentMs_ = now;
    writeActive_ = true;
    heartbeatWriteActive_ = true;
    ++backgroundWriteCount_;
    try {
        asio::co_spawn(
            transport_.executor(),
            taskAsAwaitable(writeHeartbeatPing()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        heartbeatWriteActive_ = false;
        writeActive_ = false;
        completeBackgroundWrite();
        abortTransport();
        return;
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeHeartbeatPing() {
    try {
        co_await writeFrameNow(WebSocketOpcode::kPing, {});
    } catch (...) {
        abortTransport();
    }
    heartbeatWriteActive_ = false;
    writeActive_ = false;
    completeBackgroundWrite();
}

}  // namespace ruvia::detail
