#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<void> WebSocketConnection<Transport>::write(WebSocketOpcode opcode, std::string_view payload) {
    if (!protocol_.acceptsApplicationFrames()) {
        co_return;
    }
    co_await writeExclusive(opcode, payload);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::close(std::uint16_t code, std::string_view reason) {
    if (protocol_.closed()) {
        co_return;
    }
    co_await waitForHeartbeatWrite();
    if (writeActive_) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }
    writeActive_ = true;
    try {
        if (protocol_.acceptsApplicationFrames()) {
            protocol_.submitClose(code, reason);
            localCloseStartedMs_ = scannerEntry_.lastActiveMs();
        }
        co_await flushProtocolOutputNow();
    } catch (...) {
        writeActive_ = false;
        notifyWriteIdle();
        throw;
    }
    writeActive_ = false;
    notifyWriteIdle();

    // RFC 6455: sending Close starts, but does not complete, the handshake.
    // Keep parsing transport input until the peer Close arrives (or EOF/timeout
    // aborts this transport). The core suppresses application messages in this
    // phase while still validating frames and handling control traffic.
    if (!protocol_.closed()) {
        (void)co_await read();
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::detachAndDrainBackgroundWrites() {
    scannerEntry_.clearPeriodicCheck(this);
    while (backgroundWriteCount_ > 0) {
        (void)co_await asyncError([this](auto handler) mutable {
            backgroundWriteTimer_.async_wait(std::move(handler));
        });
    }
    if (!protocol_.closed()) {
        abortTransport();
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
    std::string_view payload) {
    co_await waitForHeartbeatWrite();
    if (writeActive_) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }

    writeActive_ = true;
    try {
        co_await writeFrameNow(opcode, payload);
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
    std::string_view payload) {
    protocol_.submitFrame(opcode, payload);
    co_await flushProtocolOutputNow();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputExclusive() {
    co_await waitForWriteIdle();
    writeActive_ = true;
    try {
        co_await flushProtocolOutputNow();
    } catch (...) {
        writeActive_ = false;
        notifyWriteIdle();
        throw;
    }
    writeActive_ = false;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputNow() {
    for (;;) {
        const auto plan = protocol_.outputPlan();
        if (plan.bytes().empty() && !plan.endsTransport()) {
            co_return;
        }
        const auto ec = co_await transport_.writeBytes(plan.bytes(), plan.disposition());
        if (ec) {
            transport_.abort();
            protocol_.abort();
            throw std::system_error(ec, "failed to write websocket bytes");
        }
        protocol_.consumeOutput(plan.bytes().size());
        scannerEntry_.touch();
        if (plan.endsTransport()) {
            protocol_.commitTransportEnd();
            co_return;
        }
    }
}

template <typename Transport>
void WebSocketConnection<Transport>::abortTransport() noexcept {
    if (protocol_.closed()) {
        return;
    }
    protocol_.abort();
    transport_.abort();
    notifyWriteIdle();
}

}  // namespace ruvia::detail
