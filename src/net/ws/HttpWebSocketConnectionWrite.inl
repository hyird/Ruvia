#pragma once

namespace ruvia::detail {

template <typename Stream>
Task<void> WebSocketConnection<Stream>::write(WebSocketOpcode opcode, std::string_view payload) {
    if (closeSent_ && opcode != WebSocketOpcode::kClose) {
        co_return;
    }
    if ((opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong || opcode == WebSocketOpcode::kClose) &&
        payload.size() > 125) {
        throw std::invalid_argument("websocket control frame is too large");
    }

    while (heartbeatWriteActive_) {
        (void)co_await asyncError([this](auto handler) mutable {
            backgroundWriteTimer_.async_wait(std::move(handler));
        });
    }

    if (writeActive_) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }

    writeActive_ = true;
    try {
        co_await writeFrameNow(opcode, payload);
    } catch (...) {
        writeActive_ = false;
        throw;
    }
    writeActive_ = false;
}

template <typename Stream>
Task<void> WebSocketConnection<Stream>::close(std::uint16_t code, std::string_view reason) {
    if (closeSent_) {
        co_return;
    }
    WebSocketClosePayload payload;
    const auto payloadSize = encodeWebSocketClosePayload(payload, code, reason);
    closeSent_ = true;
    co_await write(WebSocketOpcode::kClose, std::string_view(payload.data(), payloadSize));
}

template <typename Stream>
Task<void> WebSocketConnection<Stream>::detachAndDrainBackgroundWrites() {
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

template <typename Stream>
Task<void> WebSocketConnection<Stream>::writeFrameNow(WebSocketOpcode opcode, std::string_view payload) {
    if ((opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary) &&
        webSocketMessageExceedsLimit(payload.size(), maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size());
    std::array<asio::const_buffer, 2> buffers{
        asio::buffer(header.data(), headerSize),
        asio::buffer(payload.data(), payload.size())};
    const auto ec = co_await asyncError([this, &buffers](auto handler) mutable {
        asio::async_write(stream_, buffers, std::move(handler));
    });
    if (ec) {
        throw std::invalid_argument("failed to write websocket frame");
    }
    scannerEntry_.touch();
}

}  // namespace ruvia::detail
