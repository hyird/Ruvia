#include "ruvia/http/detail/websocket/WsConnection.h"

namespace ruvia::detail {

WsConnection::WsConnection(
    std::pmr::string& input,
    ProtocolByteLimit messageLimit,
    WebSocketDeflateNegotiation deflate)
    : input_(&input),
      messageLimit_(messageLimit),
      outBuffer_(input.get_allocator().resource()),
      assembler_(input.get_allocator().resource()),
      inboundInflated_(input.get_allocator().resource()),
      outboundDeflated_(input.get_allocator().resource()) {
    if (webSocketDeflateNegotiated(deflate)) {
        deflate_.emplace();
    }
}

WsOutputPlan WsConnection::outputPlan() const noexcept {
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (closePhase_ == ClosePhase::kTransportEndReady ||
        closePhase_ == ClosePhase::kClosed) {
        return WsOutputPlan({}, closePhase_ == ClosePhase::kTransportEndReady
            ? WsTransportDisposition::kEndTransport
            : WsTransportDisposition::kKeepOpen);
    }
    const auto bytes = std::string_view(
        outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
    const auto disposition = closePhase_ == ClosePhase::kFinalCloseQueued
        ? WsTransportDisposition::kEndTransport
        : WsTransportDisposition::kKeepOpen;
    return WsOutputPlan(bytes, disposition);
}

WsOutputConsumeStatus WsConnection::consumeOutput(std::size_t n) noexcept {
    const auto remaining = outBuffer_.size() - outOffset_;
    if (n > remaining) {
        return WsOutputConsumeStatus::kOutOfRange;
    }
    if (n < remaining) {
        outOffset_ += n;
        return WsOutputConsumeStatus::kPending;
    }

    outBuffer_.clear();
    outOffset_ = 0;
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kAwaitingPeerClose;
    } else if (closePhase_ == ClosePhase::kFinalCloseQueued) {
        closePhase_ = ClosePhase::kTransportEndReady;
    }
    return WsOutputConsumeStatus::kDrained;
}

void WsConnection::commitTransportEnd() noexcept {
    if (closePhase_ == ClosePhase::kTransportEndReady) {
        closePhase_ = ClosePhase::kClosed;
    }
}

void WsConnection::notifyTransportEof() noexcept {
    if (closePhase_ == ClosePhase::kClosed) {
        return;
    }
    closePhase_ = ClosePhase::kTransportEndReady;
}

WsAbortDisposition WsConnection::abort() noexcept {
    if (closePhase_ == ClosePhase::kClosed) {
        return WsAbortDisposition::kNoTransportAction;
    }
    closePhase_ = ClosePhase::kClosed;
    return WsAbortDisposition::kAbortTransport;
}

WsLivenessMode WsConnection::livenessMode() const noexcept {
    switch (closePhase_) {
        case ClosePhase::kOpen:
            return WsLivenessMode::kOpen;
        case ClosePhase::kLocalCloseQueued:
        case ClosePhase::kAwaitingPeerClose:
            return WsLivenessMode::kAwaitingPeerClose;
        case ClosePhase::kFinalCloseQueued:
        case ClosePhase::kTransportEndReady:
        case ClosePhase::kClosed:
            return WsLivenessMode::kInactive;
    }
    return WsLivenessMode::kInactive;
}

void WsConnection::appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1) {
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1);
    outBuffer_.append(header.data(), headerSize);
    outBuffer_.append(payload.data(), payload.size());
}

void WsConnection::fail(std::uint16_t code, std::string_view reason) {
    if (closePhase_ == ClosePhase::kOpen) {
        const auto payload = encodeWebSocketClosePayload(code, reason);
        const auto* encoded = payload.encoded();
        if (encoded == nullptr) {
            return;
        }
        appendFrame(WebSocketOpcode::kClose, encoded->bytes());
        closePhase_ = ClosePhase::kFinalCloseQueued;
        return;
    }
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kFinalCloseQueued;
    } else if (closePhase_ == ClosePhase::kAwaitingPeerClose) {
        closePhase_ = ClosePhase::kTransportEndReady;
    }
}

void WsConnection::receivePeerClose() noexcept {
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kFinalCloseQueued;
    } else if (closePhase_ == ClosePhase::kAwaitingPeerClose) {
        closePhase_ = ClosePhase::kTransportEndReady;
    }
}

WsFrameSubmitStatus WsConnection::submitFrame(
    WebSocketOpcode opcode,
    std::string_view payload) {
    if (closePhase_ != ClosePhase::kOpen) {
        return WsFrameSubmitStatus::kNotOpen;
    }

    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    const bool controlFrame = opcode == WebSocketOpcode::kPing ||
        opcode == WebSocketOpcode::kPong;
    if (!dataFrame && !controlFrame) {
        return WsFrameSubmitStatus::kInvalidOpcode;
    }
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), messageLimit_)) {
        return WsFrameSubmitStatus::kMessageTooLarge;
    }
    if (controlFrame && payload.size() > 125) {
        return WsFrameSubmitStatus::kControlFrameTooLarge;
    }
    bool rsv1 = false;
    if (dataFrame && deflate_.has_value()) {
        outboundDeflated_.clear();
        if (deflate_->compress(payload, outboundDeflated_) && outboundDeflated_.size() < payload.size()) {
            payload = std::string_view(outboundDeflated_.data(), outboundDeflated_.size());
            rsv1 = true;
        }
    }
    appendFrame(opcode, payload, rsv1);
    return WsFrameSubmitStatus::kAccepted;
}

WsCloseSubmitStatus WsConnection::submitClose(
    std::uint16_t code,
    std::string_view reason) {
    if (closePhase_ == ClosePhase::kClosed) {
        return WsCloseSubmitStatus::kClosed;
    }
    if (closePhase_ != ClosePhase::kOpen) {
        return WsCloseSubmitStatus::kAlreadyClosing;
    }
    const auto payload = encodeWebSocketClosePayload(code, reason);
    if (const auto* failure = payload.failure()) {
        switch (failure->error()) {
            case WebSocketClosePayloadEncodeError::kInvalidCode:
                return WsCloseSubmitStatus::kInvalidCode;
            case WebSocketClosePayloadEncodeError::kInvalidReason:
                return WsCloseSubmitStatus::kInvalidReason;
            case WebSocketClosePayloadEncodeError::kReasonTooLarge:
                return WsCloseSubmitStatus::kReasonTooLarge;
        }
    }
    appendFrame(WebSocketOpcode::kClose, payload.encoded()->bytes());
    closePhase_ = ClosePhase::kLocalCloseQueued;
    return WsCloseSubmitStatus::kAccepted;
}

std::optional<WsEvent> WsConnection::poll() {
    inboundInflated_.clear();
    if (closePhase_ == ClosePhase::kFinalCloseQueued ||
        closePhase_ == ClosePhase::kTransportEndReady ||
        closePhase_ == ClosePhase::kClosed) {
        return WsEvent::makeTransportEnd();
    }

    const auto protocolFailureEvent = [this](WebSocketProtocolFailure failure) {
        const auto closeCode = webSocketProtocolFailureCloseCode(failure);
        fail(closeCode);
        return WsEvent::protocolError(closeCode);
    };

    for (;;) {
        const auto read = webSocketTryReadFrame(
            *input_, inputOffset_, pendingCompactUntil_, messageLimit_, deflate_.has_value());
        if (read.needInput() != nullptr) {
            return std::nullopt;
        }
        if (const auto* failure = read.failure()) {
            return protocolFailureEvent(failure->error());
        }

        const auto& frame = *read.frame();
        const auto inbound = assembler_.accept(frame, messageLimit_);
        if (const auto* failure = inbound.failure()) {
            return protocolFailureEvent(failure->error());
        }
        if (inbound.continueReading() != nullptr) {
            continue;
        }
        if (const auto* control = inbound.controlFrame()) {
            const auto payload = control->payload();
            if (control->opcode() == WebSocketOpcode::kPing) {
                // RFC 6455 requires Pong until a peer Close has arrived. A Pong
                // is a control frame, so it remains legal while a locally
                // initiated Close waits for its peer response.
                appendFrame(WebSocketOpcode::kPong, payload);
                return WsEvent::ping(payload);
            }
            if (control->opcode() == WebSocketOpcode::kPong) {
                return WsEvent::pong(payload);
            }
            if (control->opcode() == WebSocketOpcode::kClose) {
                std::uint16_t code = 1005;
                if (payload.size() >= 2) {
                    code = readWebSocketUint16(payload.data());
                }
                const auto reason = payload.size() > 2
                    ? payload.substr(2)
                    : std::string_view{};
                if (closePhase_ == ClosePhase::kOpen) {
                    appendFrame(WebSocketOpcode::kClose, payload);
                    closePhase_ = ClosePhase::kFinalCloseQueued;
                } else {
                    receivePeerClose();
                }
                return WsEvent::close(code, reason);
            }
            return protocolFailureEvent(
                WebSocketProtocolFailure::kProtocolError);
        }

        const auto& inboundMessage = *inbound.message();
        const auto& message = inboundMessage.message();
        if (inboundMessage.contentEncoding() ==
            WebSocketInboundContentEncoding::kIdentity) {
            if (closePhase_ != ClosePhase::kOpen) {
                continue;
            }
            return WsEvent::message(message.opcode(), message.payload());
        }

        // decompress() only appends, so the buffer must be emptied per MESSAGE, not
        // per poll(): one poll() drains several frames, and a message suppressed
        // during the closing handshake (below) returns via `continue` with its bytes
        // still here. Inheriting them would make the next message's UTF-8 check read
        // the concatenation, and would charge its decompression-bomb limit for both.
        inboundInflated_.clear();
        const auto inflateResult = deflate_.has_value()
            ? deflate_->decompress(
                message.payload(), inboundInflated_, messageLimit_)
            : WebSocketInflateResult::kError;
        if (inflateResult == WebSocketInflateResult::kTooLarge) {
            return protocolFailureEvent(
                WebSocketProtocolFailure::kMessageTooLarge);
        }
        if (inflateResult != WebSocketInflateResult::kOk) {
            return protocolFailureEvent(
                WebSocketProtocolFailure::kProtocolError);
        }
        const auto view = std::string_view(
            inboundInflated_.data(), inboundInflated_.size());
        if (message.opcode() == WebSocketOpcode::kText && !isValidUtf8(view)) {
            return protocolFailureEvent(
                WebSocketProtocolFailure::kInvalidPayloadData);
        }
        if (closePhase_ != ClosePhase::kOpen) {
            continue;
        }
        return WsEvent::message(message.opcode(), view);
    }
}

}  // namespace ruvia::detail
