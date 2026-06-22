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
Task<void> WebSocketConnection<Stream>::ensure(std::size_t bytes) {
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
            throw std::invalid_argument("websocket connection closed");
        }
        buffer_.resize(oldSize + bytesRead);
        scannerEntry_.touch();
    }
}

template <typename Stream>
void WebSocketConnection<Stream>::compactConsumedFrame() {
    compactWebSocketReadBuffer(buffer_, offset_, pendingCompactUntil_);
}

template <typename Stream>
Task<std::optional<typename WebSocketConnection<Stream>::Frame>> WebSocketConnection<Stream>::readFrame() {
    compactConsumedFrame();
    co_await ensure(2);
    const auto first = static_cast<unsigned char>(buffer_[offset_]);
    const auto second = static_cast<unsigned char>(buffer_[offset_ + 1]);
    WebSocketFrameStart frameStart;
    std::uint64_t length = second & 0x7FU;
    std::size_t headerSize = 2;

    if (!decodeWebSocketFrameStart(first, second, frameStart)) {
        throw std::invalid_argument("invalid websocket frame");
    }
    if (length == 126) {
        co_await ensure(headerSize + 2);
        length = readWebSocketUint16(buffer_.data() + offset_ + headerSize);
        headerSize += 2;
    } else if (length == 127) {
        co_await ensure(headerSize + 8);
        if (!readWebSocketUint64(buffer_.data() + offset_ + headerSize, length)) {
            throw std::invalid_argument("invalid websocket frame length");
        }
        headerSize += 8;
    }
    if (isInvalidWebSocketControlFrame(frameStart, length)) {
        throw std::invalid_argument("invalid websocket control frame");
    }
    if (webSocketFrameLengthExceedsLimit(length, maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    if (webSocketMaskedFrameReadSizeOverflows(length, headerSize)) {
        throw std::invalid_argument("invalid websocket frame length");
    }
    co_await ensure(headerSize + 4 + static_cast<std::size_t>(length));

    const auto maskOffset = offset_ + headerSize;
    const auto payloadOffset = maskOffset + 4;
    const auto payloadSize = static_cast<std::size_t>(length);
    auto* payload = buffer_.data() + payloadOffset;
    decodeMaskedWebSocketPayload(payload, payloadSize, buffer_.data() + maskOffset);
    const auto payloadView = std::string_view(payload, payloadSize);
    if (frameStart.opcode == WebSocketOpcode::kClose) {
        validateWebSocketClosePayload(payloadView);
    }
    offset_ = payloadOffset + static_cast<std::size_t>(length);
    pendingCompactUntil_ = offset_;
    co_return Frame{
        .opcode = frameStart.opcode,
        .payload = payloadView,
        .fin = frameStart.fin,
        .continuation = frameStart.continuation};
}

}  // namespace ruvia::detail
