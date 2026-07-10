#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<void> WebSocketConnection<Transport>::write(WebSocketOpcode opcode, std::string_view payload) {
    if (closeSent_ && opcode != WebSocketOpcode::kClose) {
        co_return;
    }
    co_await writeExclusive(opcode, payload, false);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::close(std::uint16_t code, std::string_view reason) {
    if (closeSent_) {
        co_return;
    }
    co_await waitForHeartbeatWrite();
    if (writeActive_) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }
    writeActive_ = true;
    try {
        protocol_.submitClose(code, reason);
        closeSent_ = true;
        co_await flushProtocolOutputNow(true);
    } catch (...) {
        writeActive_ = false;
        notifyWriteIdle();
        throw;
    }
    writeActive_ = false;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::detachAndDrainBackgroundWrites() {
    scannerEntry_.clearPeriodicCheck(this);
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
Task<void> WebSocketConnection<Transport>::waitForWriteIdle() {
    while (writeActive_) {
        backgroundWriteTimer_.expires_at((asio::steady_timer::time_point::max)());
        (void)co_await asyncError([this](auto handler) mutable {
            backgroundWriteTimer_.async_wait(std::move(handler));
        });
    }
}

template <typename Transport>
void WebSocketConnection<Transport>::notifyWriteIdle() noexcept {
    std::error_code ignored;
    backgroundWriteTimer_.cancel(ignored);
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
        notifyWriteIdle();
        throw;
    }
    writeActive_ = false;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeFrameNow(
    WebSocketOpcode opcode,
    std::string_view payload,
    bool endStream) {
    protocol_.submitFrame(opcode, payload);
    co_await flushProtocolOutputNow(endStream);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputExclusive(bool endStream) {
    co_await waitForWriteIdle();
    writeActive_ = true;
    try {
        co_await flushProtocolOutputNow(endStream);
    } catch (...) {
        writeActive_ = false;
        notifyWriteIdle();
        throw;
    }
    writeActive_ = false;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputNow(bool endStream) {
    while (protocol_.wantsWrite()) {
        const auto output = protocol_.pendingOutput();
        const auto ec = co_await transport_.writeFrame(output, {}, endStream);
        if (ec) {
            throw std::invalid_argument("failed to write websocket frame");
        }
        protocol_.consumeOutput(output.size());
        scannerEntry_.touch();
    }
}

}  // namespace ruvia::detail
