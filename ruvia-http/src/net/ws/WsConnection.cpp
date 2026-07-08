#include "WsConnection.h"

#include <utility>

namespace ruvia::detail {

WsConnection::WsConnection(std::pmr::memory_resource* resource, std::size_t maxMessageBytes)
    : resource_(resource),
      maxMessageBytes_(maxMessageBytes),
      input_(resource),
      outBuffer_(resource),
      assembler_(resource),
      events_(resource),
      messageStore_(resource) {}

// --- outbound byte buffer -----------------------------------------------------

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
    if (eventOffset_ >= events_.size()) {
        return WsEvent{};
    }
    return events_[eventOffset_++];
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
    std::pmr::string payload(resource_);
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason.data(), reason.size());
    appendFrame(WebSocketOpcode::kClose, std::string_view(payload.data(), payload.size()));
    closing_ = true;
}

// --- outbound submit ----------------------------------------------------------

void WsConnection::submitMessage(WebSocketOpcode opcode, std::string_view payload) {
    appendFrame(opcode, payload);
}

void WsConnection::submitPing(std::string_view payload) {
    appendFrame(WebSocketOpcode::kPing, payload);
}

void WsConnection::submitPong(std::string_view payload) {
    appendFrame(WebSocketOpcode::kPong, payload);
}

void WsConnection::submitClose(std::uint16_t code, std::string_view reason) {
    appendClose(code, reason);
}

// --- inbound ------------------------------------------------------------------

WsConnection::WsReadStatus WsConnection::readFrame(WebSocketFrameView& out) {
    const std::size_t available = input_.size() - inputOffset_;
    if (available < 2) {
        return WsReadStatus::kNeedMore;
    }
    const auto first = static_cast<unsigned char>(input_[inputOffset_]);
    const auto second = static_cast<unsigned char>(input_[inputOffset_ + 1]);
    WebSocketFrameStart frameStart;
    std::uint64_t length = second & 0x7FU;
    std::size_t headerSize = 2;

    // Not negotiating permessage-deflate in this slice: RSV1 frames are rejected.
    if (!decodeWebSocketFrameStart(first, second, frameStart, /*allowRsv1=*/false)) {
        throw WebSocketProtocolError(1002, "invalid websocket frame");
    }
    if (length == 126) {
        if (available < headerSize + 2) {
            return WsReadStatus::kNeedMore;
        }
        length = readWebSocketUint16(input_.data() + inputOffset_ + headerSize);
        headerSize += 2;
    } else if (length == 127) {
        if (available < headerSize + 8) {
            return WsReadStatus::kNeedMore;
        }
        if (!readWebSocketUint64(input_.data() + inputOffset_ + headerSize, length)) {
            throw WebSocketProtocolError(1002, "invalid websocket frame length");
        }
        headerSize += 8;
    }

    if (isInvalidWebSocketControlFrame(frameStart, length)) {
        throw WebSocketProtocolError(1002, "invalid websocket control frame");
    }
    if (webSocketFrameExceedsMessageLimit(frameStart.opcode, length, maxMessageBytes_)) {
        throw WebSocketProtocolError(1009, "websocket message is too large");
    }
    if (webSocketMaskedFrameReadSizeOverflows(length, headerSize)) {
        throw WebSocketProtocolError(1002, "invalid websocket frame length");
    }
    // A client frame is masked (RFC 6455 §5.1); the 4 mask bytes precede the payload.
    if (available < headerSize + 4 + static_cast<std::size_t>(length)) {
        return WsReadStatus::kNeedMore;
    }

    const auto maskOffset = inputOffset_ + headerSize;
    const auto payloadOffset = maskOffset + 4;
    const auto payloadSize = static_cast<std::size_t>(length);
    auto* payload = input_.data() + payloadOffset;
    decodeMaskedWebSocketPayload(payload, payloadSize, input_.data() + maskOffset);
    const auto payloadView = std::string_view(payload, payloadSize);
    if (frameStart.opcode == WebSocketOpcode::kClose) {
        validateWebSocketClosePayload(payloadView);
    }
    inputOffset_ = payloadOffset + payloadSize;
    out = WebSocketFrameView{
        .opcode = frameStart.opcode,
        .payload = payloadView,
        .fin = frameStart.fin,
        .continuation = frameStart.continuation,
        .rsv1 = frameStart.rsv1};
    return WsReadStatus::kFrame;
}

WsFeedResult WsConnection::feed(std::string_view in) {
    // Reclaim the prefix consumed by the previous feed at the START of this one so any
    // ping/pong/close payload views handed out as events stayed valid until now, and
    // reset the per-feed event + message storage (owner drains events after each feed).
    if (inputOffset_ > 0) {
        input_.erase(0, inputOffset_);
        inputOffset_ = 0;
    }
    events_.clear();
    eventOffset_ = 0;
    messageStore_.clear();
    input_.append(in.data(), in.size());

    if (closing_) {
        return {in.size(), WsFeedStatus::kClosed};
    }

    try {
        for (;;) {
            WebSocketFrameView frame;
            if (readFrame(frame) == WsReadStatus::kNeedMore) {
                break;
            }
            WebSocketMessage message = WebSocketMessageAccess::make(WebSocketOpcode::kText, {});
            switch (assembler_.accept(frame, maxMessageBytes_, message)) {
                case WebSocketInboundAction::kSendPong:
                    appendFrame(WebSocketOpcode::kPong, frame.payload);  // auto-Pong
                    events_.push_back(
                        WsEvent{WsEvent::Kind::kPing, WebSocketOpcode::kPing, frame.payload, 0});
                    break;
                case WebSocketInboundAction::kPongReceived:
                    events_.push_back(
                        WsEvent{WsEvent::Kind::kPong, WebSocketOpcode::kPong, frame.payload, 0});
                    break;
                case WebSocketInboundAction::kPeerClose: {
                    std::uint16_t code = 1005;  // "no status" when the Close carries none
                    if (frame.payload.size() >= 2) {
                        code = readWebSocketUint16(frame.payload.data());
                    }
                    appendFrame(WebSocketOpcode::kClose, frame.payload);  // echo (RFC §5.5.1)
                    events_.push_back(
                        WsEvent{WsEvent::Kind::kClose, WebSocketOpcode::kClose, frame.payload, code});
                    closing_ = true;
                    break;
                }
                case WebSocketInboundAction::kContinue:
                    break;
                case WebSocketInboundAction::kDeliver: {
                    // The assembler reuses one buffer across messages; copy into stable
                    // per-feed storage so this event's view survives later deliveries.
                    const auto view = message.payload();
                    std::pmr::string stored(view.data(), view.size(), resource_);
                    messageStore_.push_back(std::move(stored));
                    const auto& held = messageStore_.back();
                    events_.push_back(WsEvent{
                        WsEvent::Kind::kMessage, message.opcode(),
                        std::string_view(held.data(), held.size()), 0});
                    break;
                }
                case WebSocketInboundAction::kDeliverCompressed:
                    // permessage-deflate is a follow-up; a compressed message here means
                    // the peer used an extension we did not negotiate.
                    appendClose(1002, "compression not supported");
                    events_.push_back(
                        WsEvent{WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1002});
                    break;
                case WebSocketInboundAction::kInvalidUtf8:
                    appendClose(1007, "invalid utf-8");
                    events_.push_back(
                        WsEvent{WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, 1007});
                    break;
            }
            if (closing_) {
                break;
            }
        }
    } catch (const WebSocketProtocolError& error) {
        appendClose(error.closeCode(), {});
        events_.push_back(
            WsEvent{WsEvent::Kind::kProtocolError, WebSocketOpcode::kClose, {}, error.closeCode()});
    }

    return {in.size(), closing_ ? WsFeedStatus::kClosed : WsFeedStatus::kOk};
}

}  // namespace ruvia::detail
