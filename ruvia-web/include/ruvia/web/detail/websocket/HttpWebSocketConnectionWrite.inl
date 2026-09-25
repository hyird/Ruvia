#pragma once

namespace ruvia::detail {

template <typename Transport>
Task<void> WebSocketConnection<Transport>::write(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    requireCurrentWorker();
    return writeOwned(opcode, payload, WriteOperationLease(*this), compress);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeOwned(WebSocketOpcode opcode, std::string_view payload, WriteOperationLease writeLease, bool compress) {
    requireCurrentWorker();
    {
        WriteOperationLease activeWrite(std::move(writeLease));
        static_cast<void>(activeWrite);
        co_await writeExclusive(opcode, payload, compress);
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::close(::ruvia::WebSocketCloseOptions options) {
    requireCurrentWorker();
    if (readPhase_ == ReadPhase::kReserved) {
        throw std::logic_error("websocket close cannot overlap a pending read");
    }
    return closeOwned(options, WriteOperationLease(*this));
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::closeOwned(::ruvia::WebSocketCloseOptions options, WriteOperationLease writeLease) {
    requireCurrentWorker();
    {
        WriteOperationLease activeWrite(std::move(writeLease));
        static_cast<void>(activeWrite);
        co_await waitForHeartbeatWrite();
        const auto reason = options.reason.view();
        bool flushOutput = false;
        bool awaitPeerClose = false;
        {
            WriteGuard writeGuard(*this, WritePhase::kApplication);
            switch (protocol_.submitClose(options.code, reason)) {
                case WebSocketServerCloseSubmitStatus::kAccepted:
                    flushOutput = true;
                    awaitPeerClose = true;
                    break;
                case WebSocketServerCloseSubmitStatus::kAlreadyClosing:
                    flushOutput = true;
                    break;
                case WebSocketServerCloseSubmitStatus::kClosed:
                    break;
                case WebSocketServerCloseSubmitStatus::kInvalidCode:
                    throw std::invalid_argument("invalid websocket close code");
                case WebSocketServerCloseSubmitStatus::kInvalidReason:
                    throw std::invalid_argument("invalid websocket close reason");
                case WebSocketServerCloseSubmitStatus::kReasonTooLarge:
                    throw std::invalid_argument("websocket close reason is too large");
            }
            if (flushOutput) {
                co_await flushProtocolOutputNow();
            }
            if (awaitPeerClose && protocol_.livenessMode() == WebSocketLivenessMode::kAwaitingPeerClose) {
                // The timeout bounds the peer's response window, so commit it only
                // after the local Close bytes have reached the transport. The
                // successful flush touched the scanner with the current coarse
                // worker timestamp.
                livenessState_ = WebSocketAwaitingPeerClose(scannerEntry_.lastActiveMs());
            }
        }

        // RFC 6455: sending Close starts, but does not complete, the handshake.
        // Keep parsing transport input until the peer Close arrives (or EOF/timeout
        // aborts this transport). The core suppresses application messages in this
        // phase while still validating frames and handling control traffic.
        if (awaitPeerClose) {
            if (readPhase_ == ReadPhase::kActive) {
                while (readPhase_ == ReadPhase::kActive) {
                    co_await readerDoneSignal_.wait();
                }
            } else {
                (void)co_await read();
            }
        }
    }
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::detachAndDrainWrites() {
    requireCurrentWorker();
    periodicCheck_.reset();
    // Teardown first cancels the transport operation that owns a suspended
    // write, then joins that write before the connection storage disappears.
    // Application writes can outlive the handler through the public facade just
    // as heartbeat writes can outlive the scanner callback, so both phases are
    // part of the same structured drain. Force a transport abort when active
    // I/O remains even if the protocol already committed its normal close.
    const bool hasActiveIo = readPhase_ == ReadPhase::kActive || writePhase_ != WritePhase::kIdle;
    abortTransport(hasActiveIo);
    while (hasOperationsToDrain()) {
        if (readPhase_ != ReadPhase::kIdle) {
            co_await readerDoneSignal_.wait();
        } else {
            co_await backgroundWriteSignal_.wait();
        }
    }
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
Task<void> WebSocketConnection<Transport>::writeExclusive(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    co_await waitForHeartbeatWrite();
    WriteGuard writeGuard(*this, WritePhase::kApplication);
    co_await writeFrameNow(opcode, payload, compress);
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::writeFrameNow(
    WebSocketOpcode opcode, std::string_view payload, bool compress) {
    switch (protocol_.submitFrame(opcode, payload, compress)) {
        case WebSocketServerFrameSubmitStatus::kAccepted:
            break;
        case WebSocketServerFrameSubmitStatus::kNotOpen:
            co_return;
        case WebSocketServerFrameSubmitStatus::kInvalidOpcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case WebSocketServerFrameSubmitStatus::kMessageTooLarge:
            throw std::invalid_argument("websocket message is too large");
        case WebSocketServerFrameSubmitStatus::kInvalidTextPayload:
            throw std::invalid_argument("websocket text payload is not valid UTF-8");
        case WebSocketServerFrameSubmitStatus::kControlFrameTooLarge:
            throw std::invalid_argument("websocket control frame is too large");
    }
    co_await flushProtocolOutputNow();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputExclusive() {
    co_await waitForWriteIdle();
    WriteGuard writeGuard(*this, WritePhase::kApplication);
    co_await flushProtocolOutputNow();
}

template <typename Transport>
Task<void> WebSocketConnection<Transport>::flushProtocolOutputNow() {
    for (;;) {
        const auto plan = protocol_.outputPlan();
        const auto disposition = plan.disposition();
        if (plan.bytes().empty() && disposition == WebSocketServerTransportDisposition::kKeepOpen) {
            co_return;
        }
        const auto ec = co_await transport_.writeBytes(plan.bytes(), disposition);
        if (ec) {
            transport_.abort();
            (void)protocol_.abort();
            throw std::system_error(ec, "failed to write websocket bytes");
        }
        if (protocol_.consumeOutput(plan.bytes().size()) != WebSocketServerOutputConsumeStatus::kDrained) {
            std::terminate();
        }
        scannerEntry_.touch();
        if (disposition == WebSocketServerTransportDisposition::kEndTransport) {
            protocol_.commitTransportEnd();
            co_return;
        }
    }
}

template <typename Transport>
void WebSocketConnection<Transport>::abortTransport(bool forceTransport) noexcept {
    livenessState_ = WebSocketLivenessIdle{};
    const auto disposition = protocol_.abort();
    if (forceTransport || disposition == WebSocketServerAbortDisposition::kAbortTransport) {
        transport_.abort();
        notifyWriteIdle();
    }
}

}  // namespace ruvia::detail
