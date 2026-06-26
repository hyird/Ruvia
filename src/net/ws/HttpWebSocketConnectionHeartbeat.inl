#pragma once

namespace ruvia::detail {

template <typename Transport>
void WebSocketConnection<Transport>::completeBackgroundWrite() noexcept {
    if (backgroundWriteCount_ > 0) {
        --backgroundWriteCount_;
    }
    std::error_code ignored;
    backgroundWriteTimer_.cancel(ignored);
}

template <typename Transport>
bool WebSocketConnection<Transport>::heartbeatTick(std::int64_t now) noexcept {
    switch (webSocketHeartbeatDecision(
        heartbeatOptions_,
        closeSent_,
        awaitingPong_,
        writeActive_,
        scannerEntry_.lastActiveMs(),
        heartbeatPingSentMs_,
        now)) {
        case WebSocketHeartbeatDecision::kIdle:
            return false;
        case WebSocketHeartbeatDecision::kTimeout:
            return true;
        case WebSocketHeartbeatDecision::kSendPing:
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
        return true;
    }
    return false;
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeHeartbeatPing() {
    try {
        co_await writeFrameNow(WebSocketOpcode::kPing, {}, false);
    } catch (...) {
        closeSent_ = true;
    }
    heartbeatWriteActive_ = false;
    writeActive_ = false;
    completeBackgroundWrite();
}

}  // namespace ruvia::detail
