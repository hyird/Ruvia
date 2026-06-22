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
void Http2WebSocketConnection<Session>::compactConsumedFrame() {
    compactWebSocketReadBuffer(buffer_, offset_, pendingCompactUntil_);
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
Task<std::optional<typename Http2WebSocketConnection<Session>::Frame>> Http2WebSocketConnection<Session>::readFrame() {
    compactConsumedFrame();
    if (!(co_await ensure(2))) {
        co_return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(buffer_[offset_]);
    const auto second = static_cast<unsigned char>(buffer_[offset_ + 1]);
    WebSocketFrameStart frameStart;
    std::uint64_t length = second & 0x7FU;
    std::size_t headerSize = 2;

    if (!decodeWebSocketFrameStart(first, second, frameStart)) {
        throw std::invalid_argument("invalid websocket frame");
    }
    if (length == 126) {
        if (!(co_await ensure(headerSize + 2))) {
            throw std::invalid_argument("incomplete websocket frame");
        }
        length = readWebSocketUint16(buffer_.data() + offset_ + headerSize);
        headerSize += 2;
    } else if (length == 127) {
        if (!(co_await ensure(headerSize + 8))) {
            throw std::invalid_argument("incomplete websocket frame");
        }
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
    if (!(co_await ensure(headerSize + 4 + static_cast<std::size_t>(length)))) {
        throw std::invalid_argument("incomplete websocket frame");
    }

    const auto maskOffset = offset_ + headerSize;
    const auto payloadOffset = maskOffset + 4;
    const auto payloadSize = static_cast<std::size_t>(length);
    auto* payload = buffer_.data() + payloadOffset;
    decodeMaskedWebSocketPayload(payload, payloadSize, buffer_.data() + maskOffset);
    const auto payloadView = std::string_view(payload, payloadSize);
    if (frameStart.opcode == WebSocketOpcode::kClose) {
        validateWebSocketClosePayload(payloadView);
    }
    offset_ = payloadOffset + payloadSize;
    pendingCompactUntil_ = offset_;
    co_return Frame{
        .opcode = frameStart.opcode,
        .payload = payloadView,
        .fin = frameStart.fin,
        .continuation = frameStart.continuation};
}

}  // namespace ruvia::detail
