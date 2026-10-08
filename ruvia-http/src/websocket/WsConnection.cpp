#include "websocket/WsConnection.h"

#include <cstdint>
#include <stdexcept>

#include "websocket/HttpWebSocketClosePayload.h"
#include "websocket/HttpWebSocketFrameReader.h"

namespace ruvia::detail {

WsConnection::WsConnection(std::pmr::string& input, ProtocolByteLimit messageLimit,
    WebSocketCompression compression, WebSocketConnectionRole role, WebSocketMaskKeyGenerator maskKeyGenerator,
    void* maskKeyContext, int compressionLevel)
    : input_(&input),
      messageLimit_(messageLimit),
      outBuffer_(input.get_allocator().resource()),
      assembler_(input.get_allocator().resource()),
      inboundInflated_(input.get_allocator().resource()),
      outboundDeflated_(input.get_allocator().resource()),
      role_(role),
      maskKeyGenerator_(maskKeyGenerator),
      maskKeyContext_(maskKeyContext) {
    if (role_ != WebSocketConnectionRole::kServer && role_ != WebSocketConnectionRole::kClient) {
        throw std::invalid_argument("invalid WebSocket connection role");
    }
    if (compression.serverMaxWindowBits.value_or(15) < 8 || compression.serverMaxWindowBits.value_or(15) > 15 ||
        compression.clientMaxWindowBits.value_or(15) < 8 || compression.clientMaxWindowBits.value_or(15) > 15) {
        throw std::invalid_argument("invalid WebSocket compression window");
    }
    if (role_ == WebSocketConnectionRole::kClient && maskKeyGenerator_ == nullptr) {
        throw std::invalid_argument("WebSocket client connection requires a mask key generator");
    }
    if (compressionLevel < 0 || compressionLevel > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    if (webSocketDeflateNegotiated(compression)) {
        const bool server = role_ == WebSocketConnectionRole::kServer;
        const bool sendTakeover = !(server ? compression.serverNoContextTakeover : compression.clientNoContextTakeover);
        const bool receiveTakeover = !(server ? compression.clientNoContextTakeover : compression.serverNoContextTakeover);
        const int sendWindow = (server ? compression.serverMaxWindowBits : compression.clientMaxWindowBits).value_or(15);
        const int receiveWindow = (server ? compression.clientMaxWindowBits : compression.serverMaxWindowBits).value_or(15);
        deflate_.emplace(compressionLevel, sendTakeover, receiveTakeover, sendWindow, receiveWindow);
    }
}

WebSocketOutputPlan WsConnection::outputPlan() const& noexcept {
    // EOF/abort may race an async transport write. Keep the backing allocation
    // untouched until destruction, but make discarded bytes unreachable from the
    // protocol driver once transport termination has become authoritative.
    if (closePhase_ == ClosePhase::kTransportEndReady || closePhase_ == ClosePhase::kClosed) {
        return WebSocketOutputPlan({}, closePhase_ == ClosePhase::kTransportEndReady
                                           ? WebSocketTransportDisposition::kEndTransport
                                           : WebSocketTransportDisposition::kKeepOpen);
    }
    const auto bytes =
        std::string_view(outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
    const auto disposition = closePhase_ == ClosePhase::kFinalOutputQueued
                                 ? WebSocketTransportDisposition::kEndTransport
                                 : WebSocketTransportDisposition::kKeepOpen;
    return WebSocketOutputPlan(bytes, disposition);
}

WebSocketOutputConsumeStatus WsConnection::consumeOutput(std::size_t n) noexcept {
    // EOF/abort makes unsent bytes unreachable without clearing their storage:
    // an async write may still borrow it. A zero-byte transport-end write has
    // no output left to consume, regardless of those discarded backing bytes.
    if (n == 0 &&
        (closePhase_ == ClosePhase::kTransportEndReady || closePhase_ == ClosePhase::kClosed)) {
        return WebSocketOutputConsumeStatus::kDrained;
    }
    const auto remaining = outBuffer_.size() - outOffset_;
    if (n > remaining) {
        return WebSocketOutputConsumeStatus::kOutOfRange;
    }
    if (n < remaining) {
        outOffset_ += n;
        return WebSocketOutputConsumeStatus::kPending;
    }

    outBuffer_.clear();
    outOffset_ = 0;
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kAwaitingPeerClose;
    } else if (closePhase_ == ClosePhase::kFinalOutputQueued) {
        closePhase_ = ClosePhase::kTransportEndReady;
    }
    return WebSocketOutputConsumeStatus::kDrained;
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

WebSocketAbortDisposition WsConnection::abort() noexcept {
    if (closePhase_ == ClosePhase::kClosed) {
        return WebSocketAbortDisposition::kNoTransportAction;
    }
    closePhase_ = ClosePhase::kClosed;
    return WebSocketAbortDisposition::kAbortTransport;
}

WebSocketLivenessMode WsConnection::livenessMode() const noexcept {
    switch (closePhase_) {
        case ClosePhase::kOpen:
            return WebSocketLivenessMode::kOpen;
        case ClosePhase::kLocalCloseQueued:
        case ClosePhase::kAwaitingPeerClose:
            return WebSocketLivenessMode::kAwaitingPeerClose;
        case ClosePhase::kFinalOutputQueued:
        case ClosePhase::kTransportEndReady:
        case ClosePhase::kClosed:
            return WebSocketLivenessMode::kInactive;
    }
    return WebSocketLivenessMode::kInactive;
}

void WsConnection::appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1) {
    std::pmr::string ownedPayload(outBuffer_.get_allocator().resource());
    if (!payload.empty() && !outBuffer_.empty()) {
        const auto payloadAddress = reinterpret_cast<std::uintptr_t>(payload.data());
        const auto bufferAddress = reinterpret_cast<std::uintptr_t>(outBuffer_.data());
        if (payloadAddress >= bufferAddress && payloadAddress - bufferAddress < outBuffer_.size()) {
            // outputPlan() exposes a borrowed view. Copy only for this aliasing
            // case so reserve() below cannot invalidate its own append source.
            ownedPayload.assign(payload);
            payload = ownedPayload;
        }
    }
    WebSocketFrameHeader header;
    const bool masked = role_ == WebSocketConnectionRole::kClient;
    WebSocketMaskKey mask{};
    if (masked && !maskKeyGenerator_(maskKeyContext_, mask)) {
        throw std::runtime_error("failed to generate WebSocket client mask key");
    }
    const auto headerSize =
        encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1, masked);
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
        closePhase_ = ClosePhase::kFinalOutputQueued;
        return;
    }
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kFinalOutputQueued;
    } else if (closePhase_ == ClosePhase::kAwaitingPeerClose) {
        closePhase_ = outOffset_ < outBuffer_.size() ? ClosePhase::kFinalOutputQueued
                                                     : ClosePhase::kTransportEndReady;
    }
}

void WsConnection::receivePeerClose() noexcept {
    if (closePhase_ == ClosePhase::kLocalCloseQueued) {
        closePhase_ = ClosePhase::kFinalOutputQueued;
    } else if (closePhase_ == ClosePhase::kAwaitingPeerClose) {
        closePhase_ = outOffset_ < outBuffer_.size() ? ClosePhase::kFinalOutputQueued
                                                     : ClosePhase::kTransportEndReady;
    }
}

WebSocketFrameSubmitStatus WsConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload, bool compress) {
    if (closePhase_ != ClosePhase::kOpen) {
        return WebSocketFrameSubmitStatus::kNotOpen;
    }

    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    const bool controlFrame = opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong;
    if (!dataFrame && !controlFrame) {
        return WebSocketFrameSubmitStatus::kInvalidOpcode;
    }
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), messageLimit_)) {
        return WebSocketFrameSubmitStatus::kMessageTooLarge;
    }
    if (opcode == WebSocketOpcode::kText && !isValidUtf8(payload)) {
        return WebSocketFrameSubmitStatus::kInvalidTextPayload;
    }
    if (controlFrame && payload.size() > 125) {
        return WebSocketFrameSubmitStatus::kControlFrameTooLarge;
    }
    bool rsv1 = false;
    if (dataFrame && compress && deflate_.has_value()) {
        outboundDeflated_.clear();
        if (deflate_->compress(payload, outboundDeflated_) &&
            outboundDeflated_.size() < payload.size()) {
            payload = outboundDeflated_;
            rsv1 = true;
        } else {
            deflate_->discardCompression();
        }
    }
    appendFrame(opcode, payload, rsv1);
    return WebSocketFrameSubmitStatus::kAccepted;
}

WebSocketCloseSubmitStatus WsConnection::submitClose(std::uint16_t code, std::string_view reason) {
    if (closePhase_ == ClosePhase::kClosed) {
        return WebSocketCloseSubmitStatus::kClosed;
    }
    if (closePhase_ != ClosePhase::kOpen) {
        return WebSocketCloseSubmitStatus::kAlreadyClosing;
    }
    // RFC 6455 §7.4.1 reserves 1010 for a client reporting extensions that
    // were absent from the server handshake. This core emits server frames;
    // a server must reject that mismatch during the opening handshake rather
    // than initiate a Close frame with the client-only status code.
    if (role_ == WebSocketConnectionRole::kServer && code == 1010) {
        return WebSocketCloseSubmitStatus::kInvalidCode;
    }
    const auto payload = encodeWebSocketClosePayload(code, reason);
    if (const auto* failure = payload.failure()) {
        switch (failure->error()) {
            case WebSocketClosePayloadEncodeError::kInvalidCode:
                return WebSocketCloseSubmitStatus::kInvalidCode;
            case WebSocketClosePayloadEncodeError::kInvalidReason:
                return WebSocketCloseSubmitStatus::kInvalidReason;
            case WebSocketClosePayloadEncodeError::kReasonTooLarge:
                return WebSocketCloseSubmitStatus::kReasonTooLarge;
        }
    }
    appendFrame(WebSocketOpcode::kClose, payload.encoded()->bytes());
    closePhase_ = ClosePhase::kLocalCloseQueued;
    return WebSocketCloseSubmitStatus::kAccepted;
}

std::optional<WebSocketEvent> WsConnection::poll() & {
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

std::optional<WebSocketEvent> WsConnection::pollImpl() & {
    assembler_.release_completed();
    std::pmr::string(inboundInflated_.get_allocator()).swap(inboundInflated_);
    if (closePhase_ == ClosePhase::kFinalOutputQueued ||
        closePhase_ == ClosePhase::kTransportEndReady || closePhase_ == ClosePhase::kClosed) {
        return WebSocketEvent::makeTransportEnd();
    }

    const auto protocolFailureEvent = [this](WebSocketProtocolFailure failure) {
        const auto closeCode = webSocketProtocolFailureCloseCode(failure);
        fail(closeCode);
        return WebSocketEvent::protocolError(closeCode);
    };

    for (;;) {
        const auto read = webSocketTryReadFrame(*input_, inputOffset_, pendingCompactUntil_,
            messageLimit_, deflate_.has_value(), role_ == WebSocketConnectionRole::kServer);
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
                return WebSocketEvent::ping(payload);
            }
            if (control->opcode() == WebSocketOpcode::kPong) {
                return WebSocketEvent::pong(payload);
            }
            if (control->opcode() == WebSocketOpcode::kClose) {
                std::uint16_t code = 1005;
                if (payload.size() >= 2) {
                    code = readWebSocketUint16(payload.data());
                }
                const auto reason = payload.size() > 2 ? payload.substr(2) : std::string_view{};
                if (closePhase_ == ClosePhase::kOpen) {
                    appendFrame(WebSocketOpcode::kClose, payload);
                    closePhase_ = ClosePhase::kFinalOutputQueued;
                } else {
                    receivePeerClose();
                }
                return WebSocketEvent::close(code, reason);
            }
            return protocolFailureEvent(WebSocketProtocolFailure::kProtocolError);
        }

        const auto& inboundMessage = *inbound.message();
        const auto& message = inboundMessage.message();
        if (inboundMessage.contentEncoding() == WebSocketInboundContentEncoding::kIdentity) {
            if (closePhase_ != ClosePhase::kOpen) {
                continue;
            }
            return WebSocketEvent::message(message.opcode(), message.payload());
        }

        // decompress() only appends, so the buffer must be emptied per MESSAGE, not
        // per poll(): one poll() drains several frames, and a message suppressed
        // during the closing handshake (below) returns via `continue` with its bytes
        // still here. Inheriting them would make the next message's UTF-8 check read
        // the concatenation, and would charge its decompression-bomb limit for both.
        inboundInflated_.clear();
        const auto inflateResult = deflate_.has_value() ? deflate_->decompress(message.payload(),
                                                              inboundInflated_, messageLimit_)
                                                        : WebSocketInflateResult::kError;
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
        return WebSocketEvent::message(message.opcode(), view);
    }
}

}  // namespace ruvia::detail
