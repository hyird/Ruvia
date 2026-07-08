#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<void> WebSocketConnection<Transport>::write(WebSocketOpcode opcode, std::string_view payload) {
    if (closeSent_ && opcode != WebSocketOpcode::kClose) {
        co_return;
    }
    if ((opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong || opcode == WebSocketOpcode::kClose) &&
        payload.size() > 125) {
        throw std::invalid_argument("websocket control frame is too large");
    }
    co_await writeExclusive(opcode, payload, false);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::close(std::uint16_t code, std::string_view reason) {
    if (closeSent_) {
        co_return;
    }
    WebSocketClosePayload payload;
    const auto payloadSize = encodeWebSocketClosePayload(payload, code, reason);
    closeSent_ = true;
    co_await writeExclusive(WebSocketOpcode::kClose, std::string_view(payload.data(), payloadSize), true);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::detachAndDrainBackgroundWrites() {
    scannerEntry_.clearWebSocketHeartbeat(this);
    closeSent_ = true;
    while (backgroundWriteCount_ > 0) {
        (void)co_await asyncError([this](auto handler) mutable {
            backgroundWriteTimer_.async_wait(std::move(handler));
        });
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::waitForHeartbeatWrite() {
    while (heartbeatWriteActive_) {
        (void)co_await asyncError([this](auto handler) mutable {
            backgroundWriteTimer_.async_wait(std::move(handler));
        });
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeExclusive(
    WebSocketOpcode opcode,
    std::string_view payload,
    bool endStream) {
    co_await waitForHeartbeatWrite();
    if (writeActive_) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }

    writeActive_ = true;
    try {
        co_await writeFrameNow(opcode, payload, endStream);
    } catch (...) {
        writeActive_ = false;
        throw;
    }
    writeActive_ = false;
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeFrameNow(
    WebSocketOpcode opcode,
    std::string_view payload,
    bool endStream) {
    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    // permessage-deflate: compress data frames only (never control frames), and
    // only keep the result when it actually shrinks the payload — RSV1 is
    // per-message, so sending some messages uncompressed is fine and avoids the
    // small-message expansion that DEFLATE would otherwise add.
    bool rsv1 = false;
    if (dataFrame && deflate_.has_value()) {
        outboundDeflated_.clear();
        if (deflate_->compress(payload, outboundDeflated_) && outboundDeflated_.size() < payload.size()) {
            payload = std::string_view(outboundDeflated_.data(), outboundDeflated_.size());
            rsv1 = true;
        }
    }
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1);
    const auto ec = co_await transport_.writeFrame(
        std::string_view(header.data(), headerSize), payload, endStream);
    if (ec) {
        throw std::invalid_argument("failed to write websocket frame");
    }
    scannerEntry_.touch();
}

}  // namespace ruvia::detail
