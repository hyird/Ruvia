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

std::string_view WsConnection::pendingOutput() const noexcept {
    return std::string_view(outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
}

void WsConnection::consumeOutput(std::size_t n) noexcept {
    outOffset_ += n;
    if (outOffset_ >= outBuffer_.size()) {
        outBuffer_.clear();
        outOffset_ = 0;
    }
}

WsEvent WsConnection::nextEvent() {
    if (!eventPending_) {
        return {};
    }
    eventPending_ = false;
    return event_;
}

void WsConnection::appendFrame(WebSocketOpcode opcode, std::string_view payload, bool rsv1) {
    WebSocketFrameHeader header;
    const auto headerSize = encodeWebSocketFrameHeader(header, opcode, payload.size(), rsv1);
    outBuffer_.append(header.data(), headerSize);
    outBuffer_.append(payload.data(), payload.size());
}

void WsConnection::appendClose(std::uint16_t code, std::string_view reason) {
    if (closing_) {
        return;
    }
    WebSocketClosePayload payload;
    const auto size = encodeWebSocketClosePayload(payload, code, reason);
    appendFrame(WebSocketOpcode::kClose, std::string_view(payload.data(), size));
    closing_ = true;
}

void WsConnection::emit(WsEvent event) noexcept {
    event_ = event;
    eventPending_ = true;
}

void WsConnection::submitFrame(WebSocketOpcode opcode, std::string_view payload) {
    const bool dataFrame = opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary;
    const bool controlFrame = opcode == WebSocketOpcode::kPing ||
        opcode == WebSocketOpcode::kPong || opcode == WebSocketOpcode::kClose;
    if (dataFrame && webSocketMessageExceedsLimit(payload.size(), maxMessageBytes_)) {
        throw std::invalid_argument("websocket message is too large");
    }
    if (controlFrame && payload.size() > 125) {
        throw std::invalid_argument("websocket control frame is too large");
    }
    if (opcode == WebSocketOpcode::kClose) {
        validateWebSocketClosePayload(payload);
        closing_ = true;
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
    appendClose(code, reason);
}

WsFeedStatus WsConnection::feed() {
    eventPending_ = false;
    inboundInflated_.clear();
    if (closing_) {
        return WsFeedStatus::kClosed;
    }

    try {
        for (;;) {
            auto read = webSocketTryReadFrame(
                *input_, inputOffset_, pendingCompactUntil_, maxMessageBytes_, deflate_.has_value());
            if (read.status == WebSocketFrameReadStatus::kNeedMore) {
                return WsFeedStatus::kOk;
            }

            const auto& frame = *read.frame;
            auto message = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
            switch (assembler_.accept(frame, maxMessageBytes_, message)) {
                case WebSocketInboundAction::kSendPong:
                    submitPong(frame.payload);
                    emit({WsEvent::Kind::kPing, WebSocketOpcode::kPing, frame.payload, 0});
                    return WsFeedStatus::kOk;
                case WebSocketInboundAction::kPongReceived:
                    emit({WsEvent::Kind::kPong, WebSocketOpcode::kPong, frame.payload, 0});
                    return WsFeedStatus::kOk;
                case WebSocketInboundAction::kPeerClose: {
                    std::uint16_t code = 1005;
                    if (frame.payload.size() >= 2) {
                        code = readWebSocketUint16(frame.payload.data());
                    }
                    submitFrame(WebSocketOpcode::kClose, frame.payload);
                    emit({WsEvent::Kind::kClose, WebSocketOpcode::kClose, frame.payload, code});
                    return WsFeedStatus::kClosed;
                }
                case WebSocketInboundAction::kContinue:
                    continue;
                case WebSocketInboundAction::kDeliver:
                    emit({WsEvent::Kind::kMessage, message.opcode(), message.payload(), 0});
                    return WsFeedStatus::kOk;
                case WebSocketInboundAction::kDeliverCompressed: {
                    const auto result = deflate_.has_value()
                        ? deflate_->decompress(message.payload(), inboundInflated_, maxMessageBytes_)
                        : WebSocketInflateResult::kError;
                    if (result == WebSocketInflateResult::kTooLarge) {
                        appendClose(1009, "message too large");
                        emit({WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1009});
                        return WsFeedStatus::kClosed;
                    }
                    if (result != WebSocketInflateResult::kOk) {
                        appendClose(1002, "decompression failed");
                        emit({WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1002});
                        return WsFeedStatus::kClosed;
                    }
                    const auto view = std::string_view(inboundInflated_.data(), inboundInflated_.size());
                    if (message.opcode() == WebSocketOpcode::kText && !isValidUtf8(view)) {
                        appendClose(1007, "invalid utf-8");
                        emit({WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1007});
                        return WsFeedStatus::kClosed;
                    }
                    emit({WsEvent::Kind::kMessage, message.opcode(), view, 0});
                    return WsFeedStatus::kOk;
                }
                case WebSocketInboundAction::kInvalidUtf8:
                    appendClose(1007, "invalid utf-8");
                    emit({WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1007});
                    return WsFeedStatus::kClosed;
            }
        }
    } catch (const WebSocketProtocolError& error) {
        appendClose(error.closeCode(), {});
        emit({WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, error.closeCode()});
        return WsFeedStatus::kClosed;
    }
}

}  // namespace ruvia::detail
