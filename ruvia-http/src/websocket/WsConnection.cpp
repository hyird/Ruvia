#include "ruvia/http/detail/websocket/WsConnection.h"

#include <stdexcept>

namespace ruvia::detail {

WsConnection::WsConnection(
    std::pmr::string& input,
    std::size_t maxMessageBytes,
    bool permessageDeflate)
    : input_(&input),
      maxMessageBytes_(maxMessageBytes),
      outBuffer_(input.get_allocator().resource()),
      assembler_(input.get_allocator().resource()),
      inboundInflated_(input.get_allocator().resource()),
      outboundDeflated_(input.get_allocator().resource()) {
    if (permessageDeflate) {
        deflate_.emplace();
    }
}

WsOutputPlan WsConnection::outputPlan() const noexcept {
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (closePhase_ == WsClosePhase::kTransportEndReady ||
        closePhase_ == WsClosePhase::kClosed) {
        return WsOutputPlan({}, closePhase_ == WsClosePhase::kTransportEndReady
            ? WsTransportDisposition::kEndTransport
            : WsTransportDisposition::kKeepOpen);
    }
    const auto bytes = std::string_view(
        outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
    const auto disposition = transportEndPending()
        ? WsTransportDisposition::kEndTransport
        : WsTransportDisposition::kKeepOpen;
    return WsOutputPlan(bytes, disposition);
}

void WsConnection::consumeOutput(std::size_t n) noexcept {
    const auto remaining = outBuffer_.size() - outOffset_;
    outOffset_ += n < remaining ? n : remaining;
    if (outOffset_ >= outBuffer_.size()) {
        outBuffer_.clear();
        outOffset_ = 0;
        if (closePhase_ == WsClosePhase::kLocalCloseQueued) {
            closePhase_ = WsClosePhase::kAwaitingPeerClose;
        } else if (closePhase_ == WsClosePhase::kFinalCloseQueued) {
            closePhase_ = WsClosePhase::kTransportEndReady;
        }
    }
}

void WsConnection::commitTransportEnd() noexcept {
    if (closePhase_ == WsClosePhase::kTransportEndReady) {
        closePhase_ = WsClosePhase::kClosed;
    }
}

void WsConnection::notifyTransportEof() noexcept {
    if (closePhase_ == WsClosePhase::kClosed) {
        return;
    }
    closePhase_ = WsClosePhase::kTransportEndReady;
}

void WsConnection::abort() noexcept {
    closePhase_ = WsClosePhase::kClosed;
}

void WsConnection::appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1) {
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1);
    outBuffer_.append(header.data(), headerSize);
    outBuffer_.append(payload.data(), payload.size());
}

void WsConnection::fail(std::uint16_t code, std::string_view reason) {
    if (closePhase_ == WsClosePhase::kOpen) {
        WebSocketClosePayload payload;
        const auto size = encodeWebSocketClosePayload(payload, code, reason);
        appendFrame(WebSocketOpcode::kClose, std::string_view(payload.data(), size));
        closePhase_ = WsClosePhase::kFinalCloseQueued;
        return;
    }
    if (closePhase_ == WsClosePhase::kLocalCloseQueued) {
        closePhase_ = WsClosePhase::kFinalCloseQueued;
    } else if (closePhase_ == WsClosePhase::kAwaitingPeerClose) {
        closePhase_ = WsClosePhase::kTransportEndReady;
    }
}

void WsConnection::receivePeerClose() noexcept {
    if (closePhase_ == WsClosePhase::kLocalCloseQueued) {
        closePhase_ = WsClosePhase::kFinalCloseQueued;
    } else if (closePhase_ == WsClosePhase::kAwaitingPeerClose) {
        closePhase_ = WsClosePhase::kTransportEndReady;
    }
}

void WsConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload) {
    if (opcode == WebSocketOpcode::kClose) {
        if (closePhase_ != WsClosePhase::kOpen) {
            return;
        }
        if (webSocketClosePayloadFailure(payload).has_value()) {
            throw std::invalid_argument("invalid websocket close payload");
        }
        appendFrame(opcode, payload);
        closePhase_ = WsClosePhase::kLocalCloseQueued;
        return;
    }
    if (closePhase_ != WsClosePhase::kOpen) {
        throw std::logic_error("cannot submit a websocket frame after Close");
    }

    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    const bool controlFrame = opcode == WebSocketOpcode::kPing ||
        opcode == WebSocketOpcode::kPong || opcode == WebSocketOpcode::kClose;
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    if (controlFrame && payload.size() > 125) {
        throw std::invalid_argument("websocket control frame is too large");
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
}

void WsConnection::submitMessage(WebSocketOpcode opcode, std::string_view payload) {
    submitFrame(opcode, payload);
}

void WsConnection::submitPing(std::string_view payload) {
    submitFrame(WebSocketOpcode::kPing, payload);
}

void WsConnection::submitPong(std::string_view payload) {
    submitFrame(WebSocketOpcode::kPong, payload);
}

void WsConnection::submitClose(std::uint16_t code, std::string_view reason) {
    if (closePhase_ != WsClosePhase::kOpen) {
        return;
    }
    WebSocketClosePayload payload;
    const auto size = encodeWebSocketClosePayload(payload, code, reason);
    appendFrame(WebSocketOpcode::kClose, std::string_view(payload.data(), size));
    closePhase_ = WsClosePhase::kLocalCloseQueued;
}

std::optional<WsEvent> WsConnection::poll() {
    inboundInflated_.clear();
    if (transportEndPending() || closed()) {
        return WsEvent::makeTransportEnd();
    }

    const auto protocolFailureEvent = [this](WebSocketProtocolFailure failure) {
        const auto closeCode = webSocketProtocolFailureCloseCode(failure);
        fail(closeCode);
        return WsEvent::protocolError(closeCode);
    };

    for (;;) {
        const auto read = webSocketTryReadFrame(
            *input_, inputOffset_, pendingCompactUntil_, maxMessageBytes_, deflate_.has_value());
        if (read.needInput() != nullptr) {
            return std::nullopt;
        }
        if (const auto* failure = read.failure()) {
            return protocolFailureEvent(failure->error());
        }

        const auto& frame = *read.frame();
        const auto inbound = assembler_.accept(frame, maxMessageBytes_);
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
                if (closePhase_ == WsClosePhase::kOpen) {
                    appendFrame(WebSocketOpcode::kClose, payload);
                    closePhase_ = WsClosePhase::kFinalCloseQueued;
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
            if (closePhase_ != WsClosePhase::kOpen) {
                continue;
            }
            return WsEvent::message(message.opcode(), message.payload());
        }

        const auto inflateResult = deflate_.has_value()
            ? deflate_->decompress(
                message.payload(), inboundInflated_, maxMessageBytes_)
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
        if (closePhase_ != WsClosePhase::kOpen) {
            continue;
        }
        return WsEvent::message(message.opcode(), view);
    }
}

}  // namespace ruvia::detail
