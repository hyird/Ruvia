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
    if (scannerEntry_.webSocketTarget == this) {
        scannerEntry_.webSocketTarget = nullptr;
        scannerEntry_.webSocketTick = nullptr;
    }
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
    if ((opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary) &&
        webSocketMessageExceedsLimit(payload.size(), maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size());
    const auto ec = co_await transport_.writeFrame(
        std::string_view(header.data(), headerSize), payload, endStream);
    if (ec) {
        throw std::invalid_argument("failed to write websocket frame");
    }
    scannerEntry_.touch();
}

}  // namespace ruvia::detail
