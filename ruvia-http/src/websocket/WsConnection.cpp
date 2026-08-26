#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameReader.h"

#include <cstdint>
#include <stdexcept>

#include "ruvia/http/detail/websocket/frame/HttpWebSocketClosePayload.h"

namespace ruvia::detail {

WsConnection::WsConnection(std::pmr::string& input, ProtocolByteLimit messageLimit, WebSocketCompression compression, WsConnectionRole role, WsMaskKeyGenerator maskKeyGenerator, void* maskKeyContext)
    : input_(&input),
      messageLimit_(messageLimit),
      outBuffer_(input.get_allocator().resource()),
      assembler_(input.get_allocator().resource()),
      inboundInflated_(input.get_allocator().resource()),
      outboundDeflated_(input.get_allocator().resource()),
      role_(role),
      maskKeyGenerator_(maskKeyGenerator),
      maskKeyContext_(maskKeyContext) {
    if (role_ == WsConnectionRole::kClient && maskKeyGenerator_ == nullptr) {
        throw std::invalid_argument("WebSocket client connection requires a mask key generator");
    }
    if (webSocketDeflateNegotiated(compression)) {
        deflate_.emplace();
    }
}

WsOutputPlan WsConnection::outputPlan() const& noexcept {
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (closePhase_ == ClosePhase::kTransportEndReady || closePhase_ == ClosePhase::kClosed) {
        return WsOutputPlan({}, closePhase_ == ClosePhase::kTransportEndReady ? WsTransportDisposition::kEndTransport : WsTransportDisposition::kKeepOpen);
    }
    const auto bytes = std::string_view(outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
    const auto disposition = closePhase_ == ClosePhase::kFinalCloseQueued ? WsTransportDisposition::kEndTransport : WsTransportDisposition::kKeepOpen;
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
    std::pmr::string ownedPayload(outBuffer_.get_allocator().resource());
    if (!payload.empty() && !outBuffer_.empty()) {
        const auto payloadAddress = reinterpret_cast<std::uintptr_t>(payload.data());
        const auto bufferAddress = reinterpret_cast<std::uintptr_t>(outBuffer_.data());
        if (payloadAddress >= bufferAddress &&
            payloadAddress - bufferAddress < outBuffer_.size()) {
            // outputPlan() exposes a borrowed view. Copy only for this aliasing
            // case so reserve() below cannot invalidate its own append source.
            ownedPayload.assign(payload);
            payload = ownedPayload;
        }
    }
    WebSocketFrameHeader header;
    const bool masked = role_ == WsConnectionRole::kClient;
    WsMaskKey mask{};
    if (masked && !maskKeyGenerator_(maskKeyContext_, mask)) {
        throw std::runtime_error("failed to generate WebSocket client mask key");
    }
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1, masked);
    const auto maskSize = masked ? mask.size() : std::size_t{0};
    if (headerSize > outBuffer_.max_size() - outBuffer_.size() ||
        maskSize > outBuffer_.max_size() - outBuffer_.size() - headerSize ||
        payload.size() > outBuffer_.max_size() - outBuffer_.size() - headerSize - maskSize) {
        throw std::length_error("WebSocket output frame size overflow");
    }
    // One reserve is the transaction boundary. Once it succeeds, neither append
    // can allocate, so an exception can never publish an orphan wire header.
    outBuffer_.reserve(outBuffer_.size() + headerSize + maskSize + payload.size());
    outBuffer_.append(header.data(), headerSize);
    if (masked) {
        outBuffer_.append(mask.data(), mask.size());
        const auto payloadStart = outBuffer_.size();
        outBuffer_.append(payload.data(), payload.size());
        decodeMaskedWebSocketPayload(outBuffer_.data() + payloadStart, payload.size(), mask.data());
    } else {
        outBuffer_.append(payload.data(), payload.size());
    }
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

WsFrameSubmitStatus WsConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload) {
    if (closePhase_ != ClosePhase::kOpen) {
        return WsFrameSubmitStatus::kNotOpen;
    }

    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    const bool controlFrame = opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong;
    if (!dataFrame && !controlFrame) {
        return WsFrameSubmitStatus::kInvalidOpcode;
    }
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), messageLimit_)) {
        return WsFrameSubmitStatus::kMessageTooLarge;
    }
    if (opcode == WebSocketOpcode::kText && !isValidUtf8(payload)) {
        return WsFrameSubmitStatus::kInvalidTextPayload;
    }
    if (controlFrame && payload.size() > 125) {
        return WsFrameSubmitStatus::kControlFrameTooLarge;
    }
    bool rsv1 = false;
    if (dataFrame && deflate_.has_value()) {
        outboundDeflated_.clear();
        if (deflate_->compress(payload, outboundDeflated_) && outboundDeflated_.size() < payload.size()) {
            payload = outboundDeflated_;
            rsv1 = true;
        }
    }
    appendFrame(opcode, payload, rsv1);
    return WsFrameSubmitStatus::kAccepted;
}

WsCloseSubmitStatus WsConnection::submitClose(std::uint16_t code, std::string_view reason) {
    if (closePhase_ == ClosePhase::kClosed) {
        return WsCloseSubmitStatus::kClosed;
    }
    if (closePhase_ != ClosePhase::kOpen) {
        return WsCloseSubmitStatus::kAlreadyClosing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (role_ == WsConnectionRole::kServer && code == 1010) {
        return WsCloseSubmitStatus::kInvalidCode;
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

std::optional<WsEvent> WsConnection::poll() & {
    try {
        return pollImpl();
    } catch (...) {
        // Frame reading unmasks in place and advances the input cursor. If any
        // later assembler/deflate/output operation fails, retrying that frame is
        // no longer well-defined; make the terminal transport decision explicit.
        closePhase_ = ClosePhase::kClosed;
        throw;
    }
}

std::optional<WsEvent> WsConnection::pollImpl() & {
    inboundInflated_.clear();
    if (closePhase_ == ClosePhase::kFinalCloseQueued || closePhase_ == ClosePhase::kTransportEndReady || closePhase_ == ClosePhase::kClosed) {
        return WsEvent::makeTransportEnd();
    }

    const auto protocolFailureEvent = [this](WebSocketProtocolFailure failure) {
        const auto closeCode = webSocketProtocolFailureCloseCode(failure);
        fail(closeCode);
        return WsEvent::protocolError(closeCode);
    };

    for (;;) {
        const auto read = webSocketTryReadFrame(*input_, inputOffset_, pendingCompactUntil_, messageLimit_, deflate_.has_value(), role_ == WsConnectionRole::kServer);
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
                const auto reason = payload.size() > 2 ? payload.substr(2) : std::string_view{};
                if (closePhase_ == ClosePhase::kOpen) {
                    appendFrame(WebSocketOpcode::kClose, payload);
                    closePhase_ = ClosePhase::kFinalCloseQueued;
                } else {
                    receivePeerClose();
                }
                return WsEvent::close(code, reason);
            }
            return protocolFailureEvent(WebSocketProtocolFailure::kProtocolError);
        }

        const auto& inboundMessage = *inbound.message();
        const auto& message = inboundMessage.message();
        if (inboundMessage.contentEncoding() == WebSocketInboundContentEncoding::kIdentity) {
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
        const auto inflateResult = deflate_.has_value() ? deflate_->decompress(message.payload(), inboundInflated_, messageLimit_) : WebSocketInflateResult::kError;
        if (inflateResult == WebSocketInflateResult::kTooLarge) {
            return protocolFailureEvent(WebSocketProtocolFailure::kMessageTooLarge);
        }
        if (inflateResult != WebSocketInflateResult::kOk) {
            return protocolFailureEvent(WebSocketProtocolFailure::kProtocolError);
        }
        const std::string_view view = inboundInflated_;
        if (message.opcode() == WebSocketOpcode::kText && !isValidUtf8(view)) {
            return protocolFailureEvent(WebSocketProtocolFailure::kInvalidPayloadData);
        }
        if (closePhase_ != ClosePhase::kOpen) {
            continue;
        }
        return WsEvent::message(message.opcode(), view);
    }
}

}  // namespace ruvia::detail
