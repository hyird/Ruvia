#pragma once

namespace ruvia::detail {

template <typename Stream>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Stream>::read() {
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
                    co_await write(WebSocketOpcode::kClose, frame->payload);
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

template <typename Stream>
Task<bool> WebSocketConnection<Stream>::ensure(std::size_t bytes) {
    while (buffer_.size() - offset_ < bytes) {
        const auto oldSize = buffer_.size();
        resizePmrStringForOverwrite(buffer_, oldSize + 4096);
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, oldSize](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(buffer_.data() + oldSize, buffer_.size() - oldSize),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            buffer_.resize(oldSize);
            co_return false;
        }
        buffer_.resize(oldSize + bytesRead);
        scannerEntry_.touch();
    }
    co_return true;
}

template <typename Stream>
Task<std::optional<WebSocketFrameView>> WebSocketConnection<Stream>::readFrame() {
    return webSocketReadFrame(
        buffer_, offset_, pendingCompactUntil_, maxMessageBytes_,
        [this](std::size_t bytes) { return ensure(bytes); });
}

}  // namespace ruvia::detail
