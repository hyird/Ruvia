#pragma once

namespace ruvia::detail {

template <typename Session>
Task<std::optional<WebSocketMessage>> Http2WebSocketConnection<Session>::read() {
    for (;;) {
        auto frame = co_await readFrame();
        if (!frame) {
            co_return std::nullopt;
        }
        if (frame->opcode == WebSocketOpcode::kPing) {
            co_await write(WebSocketOpcode::kPong, frame->payload);
            continue;
        }
        if (frame->opcode == WebSocketOpcode::kPong) {
            awaitingPong_ = false;
            continue;
        }
        if (frame->opcode == WebSocketOpcode::kClose) {
            if (!closeSent_) {
                closeSent_ = true;
                co_await writeExclusive(WebSocketOpcode::kClose, frame->payload, true);
            }
            co_return std::nullopt;
        }
        if (frame->continuation) {
            if (!fragmented_) {
                throw std::invalid_argument("unexpected websocket continuation frame");
            }
            if (webSocketAppendExceedsLimit(
                    fragmentedMessage_.size(),
                    frame->payload.size(),
                    maxMessageBytes_)) {
                throw std::invalid_argument("websocket message is too large");
            }
            fragmentedMessage_.append(frame->payload.data(), frame->payload.size());
            if (!frame->fin) {
                continue;
            }
            if (fragmentedOpcode_ == WebSocketOpcode::kText && !isValidUtf8(fragmentedMessage_)) {
                co_await close(1007, "invalid utf-8");
                co_return std::nullopt;
            }
            fragmented_ = false;
            co_return WebSocketMessage{
                .opcode = fragmentedOpcode_,
                .payload = std::string_view(fragmentedMessage_.data(), fragmentedMessage_.size())};
        }
        if (frame->opcode == WebSocketOpcode::kText || frame->opcode == WebSocketOpcode::kBinary) {
            if (fragmented_) {
                throw std::invalid_argument("invalid websocket fragmented message");
            }
            if (frame->fin) {
                if (frame->opcode == WebSocketOpcode::kText && !isValidUtf8(frame->payload)) {
                    co_await close(1007, "invalid utf-8");
                    co_return std::nullopt;
                }
                co_return WebSocketMessage{.opcode = frame->opcode, .payload = frame->payload};
            }
            fragmented_ = true;
            fragmentedOpcode_ = frame->opcode;
            fragmentedMessage_.assign(frame->payload.data(), frame->payload.size());
            continue;
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
