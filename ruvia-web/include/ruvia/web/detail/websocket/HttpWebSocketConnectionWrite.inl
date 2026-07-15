#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<void> WebSocketConnection<Transport>::write(WebSocketOpcode opcode, std::string_view payload) {
    co_await writeExclusive(opcode, payload);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::close(std::uint16_t code, std::string_view reason) {
    co_await waitForHeartbeatWrite();
    if (writePhase_ != WritePhase::kIdle) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }
    writePhase_ = WritePhase::kApplication;
    bool flushOutput = false;
    bool awaitPeerClose = false;
    try {
        switch (protocol_.submitClose(code, reason)) {
            case WsCloseSubmitStatus::kAccepted:
                flushOutput = true;
                awaitPeerClose = true;
                break;
            case WsCloseSubmitStatus::kAlreadyClosing:
                flushOutput = true;
                break;
            case WsCloseSubmitStatus::kClosed:
                break;
            case WsCloseSubmitStatus::kInvalidCode:
                throw std::invalid_argument("invalid websocket close code");
            case WsCloseSubmitStatus::kInvalidReason:
                throw std::invalid_argument("invalid websocket close reason");
            case WsCloseSubmitStatus::kReasonTooLarge:
                throw std::invalid_argument("websocket close reason is too large");
        }
        if (flushOutput) {
            co_await flushProtocolOutputNow();
        }
        if (awaitPeerClose &&
            protocol_.livenessMode() == WsLivenessMode::kAwaitingPeerClose) {
            // The timeout bounds the peer's response window, so commit it only
            // after the local Close bytes have reached the transport. The
            // successful flush touched the scanner with the current coarse
            // worker timestamp.
            livenessState_ = WebSocketAwaitingPeerClose(
                scannerEntry_.lastActiveMs());
        }
    } catch (...) {
        writePhase_ = WritePhase::kIdle;
        notifyWriteIdle();
        throw;
    }
    writePhase_ = WritePhase::kIdle;
    notifyWriteIdle();

    // RFC 6455: sending Close starts, but does not complete, the handshake.
    // Keep parsing transport input until the peer Close arrives (or EOF/timeout
    // aborts this transport). The core suppresses application messages in this
    // phase while still validating frames and handling control traffic.
    if (awaitPeerClose) {
        if (readActive_) {
            while (readActive_) {
                co_await readerDoneSignal_.wait();
            }
        } else {
            (void)co_await read();
        }
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::detachAndDrainBackgroundWrites() {
    periodicCheck_.reset();
    while (writePhase_ == WritePhase::kHeartbeat) {
        co_await backgroundWriteSignal_.wait();
    }
    abortTransport();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::waitForHeartbeatWrite() {
    while (writePhase_ == WritePhase::kHeartbeat) {
        co_await backgroundWriteSignal_.wait();
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::waitForWriteIdle() {
    while (writePhase_ != WritePhase::kIdle) {
        co_await backgroundWriteSignal_.wait();
    }
}

template <typename Transport>
void WebSocketConnection<Transport>::notifyWriteIdle() noexcept {
    backgroundWriteSignal_.notify();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeExclusive(
    WebSocketOpcode opcode,
    std::string_view payload) {
    co_await waitForHeartbeatWrite();
    if (writePhase_ != WritePhase::kIdle) {
        throw std::logic_error("concurrent websocket writes are not supported");
    }

    writePhase_ = WritePhase::kApplication;
    try {
        co_await writeFrameNow(opcode, payload);
    } catch (...) {
        writePhase_ = WritePhase::kIdle;
        notifyWriteIdle();
        throw;
    }
    writePhase_ = WritePhase::kIdle;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeFrameNow(
    WebSocketOpcode opcode,
    std::string_view payload) {
    switch (protocol_.submitFrame(opcode, payload)) {
        case WsFrameSubmitStatus::kAccepted:
            break;
        case WsFrameSubmitStatus::kNotOpen:
            co_return;
        case WsFrameSubmitStatus::kInvalidOpcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case WsFrameSubmitStatus::kMessageTooLarge:
            throw std::invalid_argument("websocket message is too large");
        case WsFrameSubmitStatus::kControlFrameTooLarge:
            throw std::invalid_argument("websocket control frame is too large");
    }
    co_await flushProtocolOutputNow();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputExclusive() {
    co_await waitForWriteIdle();
    writePhase_ = WritePhase::kApplication;
    try {
        co_await flushProtocolOutputNow();
    } catch (...) {
        writePhase_ = WritePhase::kIdle;
        notifyWriteIdle();
        throw;
    }
    writePhase_ = WritePhase::kIdle;
    notifyWriteIdle();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputNow() {
    for (;;) {
        const auto plan = protocol_.outputPlan();
        const auto disposition = plan.disposition();
        if (plan.bytes().empty() &&
            disposition == WsTransportDisposition::kKeepOpen) {
            co_return;
        }
        const auto ec = co_await transport_.writeBytes(
            plan.bytes(), disposition);
        if (ec) {
            transport_.abort();
            (void)protocol_.abort();
            throw std::system_error(ec, "failed to write websocket bytes");
        }
        protocol_.consumeOutput(plan.bytes().size());
        scannerEntry_.touch();
        if (disposition == WsTransportDisposition::kEndTransport) {
            protocol_.commitTransportEnd();
            co_return;
        }
    }
}

template <typename Transport>
void WebSocketConnection<Transport>::abortTransport() noexcept {
    livenessState_ = WebSocketLivenessIdle{};
    if (protocol_.abort() == WsAbortDisposition::kAbortTransport) {
        transport_.abort();
        notifyWriteIdle();
    }
}

}  // namespace ruvia::detail
