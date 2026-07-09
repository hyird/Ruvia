#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<std::optional<WebSocketMessage>> WebSocketConnection<Transport>::read() {
    for (;;) {
        std::optional<WebSocketFrameView> frame;
        auto message = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
        WebSocketInboundAction action = WebSocketInboundAction::kContinue;
        // co_await is not allowed inside a catch handler, so record the violation
        // and send the Close after the try. A protocol violation sets a nonzero
        // close code; an abnormal mid-frame EOF ends the loop with no Close frame.
        std::uint16_t violationCloseCode = 0;
        std::string violationReason;
        try {
            frame = co_await readFrame();
            if (!frame) {
                co_return std::nullopt;
            }
            action = inbound_.accept(*frame, maxMessageBytes_, message);
        } catch (const WebSocketProtocolError& error) {
            // Wire-level violation: reply with the RFC 6455 §7.4.1 close code the
            // peer expects (1002/1007/1009) instead of the generic 1011.
            violationCloseCode = error.closeCode();
            violationReason = error.what();
        } catch (const std::invalid_argument&) {
            // Mid-frame EOF (peer vanished): abnormal closure (1006), which is
            // never sent on the wire — just end the read loop without a Close.
            co_return std::nullopt;
        }
        if (violationCloseCode != 0) {
            co_await close(violationCloseCode, violationReason);
            co_return std::nullopt;
        }
        switch (action) {
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
    for (;;) {
        auto result = webSocketTryReadFrame(
            buffer_, offset_, pendingCompactUntil_, maxMessageBytes_, permessageDeflate_);
        if (result.status == WebSocketFrameReadStatus::kFrame) {
            co_return std::move(result.frame);
        }
        if (!(co_await ensure(result.requiredBytes))) {
            if (result.cleanEofAllowed) {
                co_return std::nullopt;
            }
            throw std::invalid_argument("incomplete websocket frame");
        }
    }
}

}  // namespace ruvia::detail
