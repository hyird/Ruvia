#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::read() {
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
            case WebSocketInboundAction::kDeliverCompressed: {
                inboundInflated_.clear();
                const auto result = deflate_.has_value()
                    ? deflate_->decompress(message.payload(), inboundInflated_, maxMessageBytes_)
                    : WebSocketInflateResult::kError;
                if (result == WebSocketInflateResult::kTooLarge) {
                    co_await close(1009, "message too large");
                    co_return std::nullopt;
                }
                if (result != WebSocketInflateResult::kOk) {
                    co_await close(1002, "decompression failed");
                    co_return std::nullopt;
                }
                if (message.opcode() == WebSocketOpcode::kText && !isValidUtf8(inboundInflated_)) {
                    co_await close(1007, "invalid utf-8");
                    co_return std::nullopt;
                }
                co_return WebSocketMessageAccess::make(
                    message.opcode(),
                    std::string_view(inboundInflated_.data(), inboundInflated_.size()));
            }
            case WebSocketInboundAction::kInvalidUtf8:
                co_await close(1007, "invalid utf-8");
                co_return std::nullopt;
        }
    }
}

template <typename Transport>
Task<bool> WebSocketConnection<Transport>::ensure(std::size_t bytes) {
    while (buffer_.size() - offset_ < bytes) {
        if (!(co_await transport_.readMore(buffer_))) {
            co_return false;
        }
        scannerEntry_.touch();
    }
    co_return true;
}

template <typename Transport>
Task<std::optional<WebSocketFrameView>> WebSocketConnection<Transport>::readFrame() {
    return webSocketReadFrame(
        buffer_, offset_, pendingCompactUntil_, maxMessageBytes_, permessageDeflate_,
        [this](std::size_t bytes) { return ensure(bytes); });
}

}  // namespace ruvia::detail
