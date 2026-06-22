#pragma once

namespace ruvia::detail {

template <typename Session>
Task<std::optional<WebSocketMessage>> Http2WebSocketConnection<Session>::read() {
    for (;;) {
        auto frame = co_await readFrame();
        if (!frame) {
            co_return std::nullopt;
        }
        WebSocketMessage message;
        switch (inbound_.accept(*frame, maxMessageBytes_, message)) {
            case WebSocketInboundAction::kSendPong:
                co_await write(WebSocketOpcode::kPong, frame->payload);
                continue;
            case WebSocketInboundAction::kPongReceived:
                awaitingPong_ = false;
                continue;
            case WebSocketInboundAction::kPeerClose:
                if (!closeSent_) {
                    closeSent_ = true;
                    co_await writeExclusive(WebSocketOpcode::kClose, frame->payload, true);
                }
                co_return std::nullopt;
            case WebSocketInboundAction::kContinue:
                continue;
            case WebSocketInboundAction::kDeliver:
                co_return message;
            case WebSocketInboundAction::kInvalidUtf8:
                co_await close(1007, "invalid utf-8");
                co_return std::nullopt;
        }
    }
}

template <typename Session>
Task<bool> Http2WebSocketConnection<Session>::ensure(std::size_t bytes) {
    while (buffer_.size() - offset_ < bytes) {
        auto chunk = co_await session_.readBodyChunk(stream_.id);
        if (!chunk) {
            co_return false;
        }
        buffer_.append(chunk->data(), chunk->size());
        scannerEntry_.touch();
    }
    co_return true;
}

template <typename Session>
Task<std::optional<WebSocketFrameView>> Http2WebSocketConnection<Session>::readFrame() {
    return webSocketReadFrame(
        buffer_, offset_, pendingCompactUntil_, maxMessageBytes_,
        [this](std::size_t bytes) { return ensure(bytes); });
}

}  // namespace ruvia::detail
