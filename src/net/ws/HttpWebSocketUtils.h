#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../../http/HttpRequestFlags.h"
#include "ruvia/http/WebSocket.h"

namespace ruvia {

class HttpRequest;

namespace detail {

using WebSocketFrameHeader = std::array<char, 10>;
using WebSocketClosePayload = std::array<char, 125>;
using WebSocketAcceptKey = std::array<char, 28>;

struct WebSocketFrameStart final {
    WebSocketOpcode opcode;
    bool fin{false};
    bool continuation{false};
    bool rsv1{false};
};

[[nodiscard]] inline bool isInvalidWebSocketRawOpcode(std::uint8_t rawOpcode) noexcept {
    return (rawOpcode >= 0x3 && rawOpcode <= 0x7) || rawOpcode >= 0xB;
}

[[nodiscard]] inline bool isWebSocketControlOpcode(WebSocketOpcode opcode) noexcept {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

// allowRsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline bool decodeWebSocketFrameStart(
    unsigned char first,
    unsigned char second,
    WebSocketFrameStart& frame,
    bool allowRsv1) noexcept {
    const auto rawOpcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || (second & 0x80U) == 0 || isInvalidWebSocketRawOpcode(rawOpcode)) {
        return false;
    }
    if (rsv1 && (!allowRsv1 || rawOpcode == 0 || rawOpcode >= 0x8)) {
        return false;
    }
    frame = WebSocketFrameStart{
        .opcode = static_cast<WebSocketOpcode>(rawOpcode == 0 ? 0x1 : rawOpcode),
        .fin = (first & 0x80U) != 0,
        .continuation = rawOpcode == 0,
        .rsv1 = rsv1};
    return true;
}

[[nodiscard]] inline bool isInvalidWebSocketControlFrame(
    const WebSocketFrameStart& frame,
    std::uint64_t payloadSize) noexcept {
    return isWebSocketControlOpcode(frame.opcode) && (!frame.fin || payloadSize > 125);
}

[[nodiscard]] inline bool webSocketMessageExceedsLimit(
    std::size_t payloadSize,
    std::size_t maxMessageBytes) noexcept {
    return maxMessageBytes != 0 && payloadSize > maxMessageBytes;
}

[[nodiscard]] inline bool webSocketAppendExceedsLimit(
    std::size_t currentSize,
    std::size_t appendSize,
    std::size_t maxMessageBytes) noexcept {
    return maxMessageBytes != 0 &&
        (appendSize > maxMessageBytes || currentSize > maxMessageBytes - appendSize);
}

[[nodiscard]] inline bool webSocketFrameLengthExceedsLimit(
    std::uint64_t payloadSize,
    std::size_t maxMessageBytes) noexcept {
    if (payloadSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return webSocketMessageExceedsLimit(static_cast<std::size_t>(payloadSize), maxMessageBytes);
}

[[nodiscard]] inline bool webSocketMaskedFrameReadSizeOverflows(
    std::uint64_t payloadSize,
    std::size_t headerSize) noexcept {
    constexpr std::size_t kMaskBytes = 4;
    constexpr auto kMaxSize = (std::numeric_limits<std::size_t>::max)();
    return headerSize > kMaxSize - kMaskBytes ||
        payloadSize > static_cast<std::uint64_t>(kMaxSize - headerSize - kMaskBytes);
}

enum class WebSocketHeartbeatDecision : std::uint8_t {
    kIdle,
    kSendPing,
    kTimeout
};

[[nodiscard]] inline WebSocketHeartbeatDecision webSocketHeartbeatDecision(
    const WebSocketHeartbeatOptions& options,
    bool closeSent,
    bool awaitingPong,
    bool writeActive,
    std::int64_t lastActiveMs,
    std::int64_t heartbeatPingSentMs,
    std::int64_t now) noexcept {
    const auto pingInterval = options.pingInterval.count();
    if (pingInterval <= 0 || closeSent) {
        return WebSocketHeartbeatDecision::kIdle;
    }

    auto pongTimeout = options.pongTimeout.count();
    if (pongTimeout <= 0) {
        pongTimeout = pingInterval;
    }
    if (awaitingPong) {
        return now - heartbeatPingSentMs >= pongTimeout
            ? WebSocketHeartbeatDecision::kTimeout
            : WebSocketHeartbeatDecision::kIdle;
    }
    if (now - lastActiveMs < pingInterval || writeActive) {
        return WebSocketHeartbeatDecision::kIdle;
    }
    return WebSocketHeartbeatDecision::kSendPing;
}

[[nodiscard]] inline std::uint16_t readWebSocketUint16(const char* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) |
        static_cast<unsigned char>(data[1]));
}

[[nodiscard]] inline bool readWebSocketUint64(const char* data, std::uint64_t& value) noexcept {
    if ((static_cast<unsigned char>(data[0]) & 0x80U) != 0) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<unsigned char>(data[i]);
    }
    return true;
}

[[nodiscard]] inline std::size_t encodeWebSocketFrameHeader(
    WebSocketFrameHeader& header,
    WebSocketOpcode opcode,
    std::size_t payloadSize,
    bool rsv1 = false) noexcept {
    std::size_t headerSize = 0;
    header[headerSize++] = static_cast<char>(
        0x80U | (rsv1 ? 0x40U : 0U) | static_cast<std::uint8_t>(opcode));
    if (payloadSize <= 125) {
        header[headerSize++] = static_cast<char>(payloadSize);
    } else if (payloadSize <= 0xFFFF) {
        header[headerSize++] = static_cast<char>(126);
        header[headerSize++] = static_cast<char>((payloadSize >> 8) & 0xFF);
        header[headerSize++] = static_cast<char>(payloadSize & 0xFF);
    } else {
        header[headerSize++] = static_cast<char>(127);
        const auto size = static_cast<std::uint64_t>(payloadSize);
        for (int shift = 56; shift >= 0; shift -= 8) {
            header[headerSize++] = static_cast<char>((size >> shift) & 0xFF);
        }
    }
    return headerSize;
}

inline void decodeMaskedWebSocketPayload(char* payload, std::size_t payloadSize, const char* mask) noexcept {
    const auto m0 = static_cast<unsigned char>(mask[0]);
    const auto m1 = static_cast<unsigned char>(mask[1]);
    const auto m2 = static_cast<unsigned char>(mask[2]);
    const auto m3 = static_cast<unsigned char>(mask[3]);
    std::size_t i = 0;
    for (; i + 4 <= payloadSize; i += 4) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m0);
        payload[i + 1] = static_cast<char>(static_cast<unsigned char>(payload[i + 1]) ^ m1);
        payload[i + 2] = static_cast<char>(static_cast<unsigned char>(payload[i + 2]) ^ m2);
        payload[i + 3] = static_cast<char>(static_cast<unsigned char>(payload[i + 3]) ^ m3);
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m0);
        ++i;
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m1);
        ++i;
    }
    if (i < payloadSize) {
        payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ m2);
    }
}

inline void compactWebSocketReadBuffer(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t& pendingCompactUntil) noexcept {
    if (pendingCompactUntil == 0) {
        return;
    }

    const auto consumed = pendingCompactUntil;
    pendingCompactUntil = 0;
    if (consumed >= buffer.size()) {
        buffer.clear();
        offset = 0;
        return;
    }

    offset = consumed;
    constexpr std::size_t kCompactThresholdBytes = 16 * 1024;
    if (consumed < kCompactThresholdBytes && consumed < buffer.size() - consumed) {
        return;
    }

    const auto remaining = buffer.size() - consumed;
    std::memmove(buffer.data(), buffer.data() + consumed, remaining);
    buffer.resize(remaining);
    offset = 0;
}

void encodeWebSocketAccept(WebSocketAcceptKey& output, std::string_view key);
[[nodiscard]] std::pmr::string webSocketAccept(std::string_view key, std::pmr::memory_resource* resource);
[[nodiscard]] bool webSocketProtocolOffered(const HttpRequest& request, std::string_view protocol) noexcept;
[[nodiscard]] bool isValidWebSocketRequest(const HttpRequest& request, const HttpRequestFlags& flags) noexcept;
[[nodiscard]] bool isValidWebSocketCloseCode(std::uint16_t code) noexcept;
[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;
[[nodiscard]] std::size_t encodeWebSocketClosePayload(
    WebSocketClosePayload& payload,
    std::uint16_t code,
    std::string_view reason);
void validateWebSocketClosePayload(std::string_view payload);
[[nodiscard]] std::string_view chooseWebSocketSubprotocol(
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supported) noexcept;

enum class WebSocketInboundAction : std::uint8_t {
    kSendPong,          // reply with a Pong echoing the frame payload
    kPongReceived,      // a Pong arrived; clear the awaiting-pong state
    kPeerClose,         // peer-initiated Close; echo it then end the stream
    kContinue,          // frame consumed, nothing to deliver yet
    kDeliver,           // `out` holds a complete (already-validated) message
    kDeliverCompressed, // `out` holds a permessage-deflate message to inflate
    kInvalidUtf8        // text payload is not valid UTF-8; close with 1007
};

// Single owner of the WebSocket inbound message-reassembly state machine (RFC
// 6455 §5.4): control-frame dispatch, fragmentation across continuation frames,
// per-message size limits and UTF-8 validation. Shared by the HTTP/1.1 and
// HTTP/2 transports, which differ only in how they read frames and echo Close,
// so only those parts stay in each connection's read loop. Throws
// std::invalid_argument on protocol violations, matching the prior behavior.
class WebSocketInboundAssembler final {
public:
    explicit WebSocketInboundAssembler(std::pmr::memory_resource* resource)
        : message_(resource) {}

    template <typename FrameT>
    [[nodiscard]] WebSocketInboundAction accept(
        const FrameT& frame,
        std::size_t maxMessageBytes,
        WebSocketMessage& out) {
        if (frame.opcode == WebSocketOpcode::kPing) {
            return WebSocketInboundAction::kSendPong;
        }
        if (frame.opcode == WebSocketOpcode::kPong) {
            return WebSocketInboundAction::kPongReceived;
        }
        if (frame.opcode == WebSocketOpcode::kClose) {
            return WebSocketInboundAction::kPeerClose;
        }
        if (frame.continuation) {
            if (!fragmented_) {
                throw std::invalid_argument("unexpected websocket continuation frame");
            }
            if (webSocketAppendExceedsLimit(message_.size(), frame.payload.size(), maxMessageBytes)) {
                throw std::invalid_argument("websocket message is too large");
            }
            message_.append(frame.payload.data(), frame.payload.size());
            if (!frame.fin) {
                return WebSocketInboundAction::kContinue;
            }
            fragmented_ = false;
            out = WebSocketMessageAccess::make(
                opcode_,
                std::string_view(message_.data(), message_.size()));
            // A compressed message must be inflated before UTF-8 can be judged,
            // so defer validation to the connection.
            if (compressed_) {
                return WebSocketInboundAction::kDeliverCompressed;
            }
            if (opcode_ == WebSocketOpcode::kText && !isValidUtf8(message_)) {
                return WebSocketInboundAction::kInvalidUtf8;
            }
            return WebSocketInboundAction::kDeliver;
        }
        if (frame.opcode == WebSocketOpcode::kText || frame.opcode == WebSocketOpcode::kBinary) {
            if (fragmented_) {
                throw std::invalid_argument("invalid websocket fragmented message");
            }
            if (frame.fin) {
                out = WebSocketMessageAccess::make(frame.opcode, frame.payload);
                if (frame.rsv1) {
                    return WebSocketInboundAction::kDeliverCompressed;
                }
                if (frame.opcode == WebSocketOpcode::kText && !isValidUtf8(frame.payload)) {
                    return WebSocketInboundAction::kInvalidUtf8;
                }
                return WebSocketInboundAction::kDeliver;
            }
            fragmented_ = true;
            opcode_ = frame.opcode;
            compressed_ = frame.rsv1;
            message_.assign(frame.payload.data(), frame.payload.size());
            return WebSocketInboundAction::kContinue;
        }
        return WebSocketInboundAction::kContinue;
    }

private:
    std::pmr::string message_;
    WebSocketOpcode opcode_{WebSocketOpcode::kText};
    bool fragmented_{false};
    bool compressed_{false};
};

struct WebSocketFrameView final {
    WebSocketOpcode opcode{WebSocketOpcode::kText};
    std::string_view payload;
    bool fin{true};
    bool continuation{false};
    bool rsv1{false};
};

// Single owner of WebSocket frame decode (RFC 6455 §5.2): FIN/opcode/length
// parsing, control-frame and length-limit validation, and in-place unmasking.
// `ensure` is a callable returning an awaitable<bool>; `co_await ensure(n)`
// yields true once at least n bytes are buffered from `offset`, or false if the
// stream ended first. A clean end at a frame boundary returns std::nullopt; an
// end mid-frame throws. Shared by the HTTP/1.1 and HTTP/2 transports, which
// differ only in how `ensure` fills the buffer.
template <typename Ensure>
[[nodiscard]] Task<std::optional<WebSocketFrameView>> webSocketReadFrame(
    std::pmr::string& buffer,
    std::size_t& offset,
    std::size_t& pendingCompactUntil,
    std::size_t maxMessageBytes,
    bool permessageDeflate,
    Ensure ensure) {
    compactWebSocketReadBuffer(buffer, offset, pendingCompactUntil);
    if (!(co_await ensure(2))) {
        co_return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(buffer[offset]);
    const auto second = static_cast<unsigned char>(buffer[offset + 1]);
    WebSocketFrameStart frameStart;
    std::uint64_t length = second & 0x7FU;
    std::size_t headerSize = 2;

    if (!decodeWebSocketFrameStart(first, second, frameStart, permessageDeflate)) {
        throw std::invalid_argument("invalid websocket frame");
    }
    if (length == 126) {
        if (!(co_await ensure(headerSize + 2))) {
            throw std::invalid_argument("incomplete websocket frame");
        }
        length = readWebSocketUint16(buffer.data() + offset + headerSize);
        headerSize += 2;
    } else if (length == 127) {
        if (!(co_await ensure(headerSize + 8))) {
            throw std::invalid_argument("incomplete websocket frame");
        }
        if (!readWebSocketUint64(buffer.data() + offset + headerSize, length)) {
            throw std::invalid_argument("invalid websocket frame length");
        }
        headerSize += 8;
    }

    if (isInvalidWebSocketControlFrame(frameStart, length)) {
        throw std::invalid_argument("invalid websocket control frame");
    }
    if (webSocketFrameLengthExceedsLimit(length, maxMessageBytes)) {
        throw std::invalid_argument("websocket message is too large");
    }
    if (webSocketMaskedFrameReadSizeOverflows(length, headerSize)) {
        throw std::invalid_argument("invalid websocket frame length");
    }
    if (!(co_await ensure(headerSize + 4 + static_cast<std::size_t>(length)))) {
        throw std::invalid_argument("incomplete websocket frame");
    }

    const auto maskOffset = offset + headerSize;
    const auto payloadOffset = maskOffset + 4;
    const auto payloadSize = static_cast<std::size_t>(length);
    auto* payload = buffer.data() + payloadOffset;
    decodeMaskedWebSocketPayload(payload, payloadSize, buffer.data() + maskOffset);
    const auto payloadView = std::string_view(payload, payloadSize);
    if (frameStart.opcode == WebSocketOpcode::kClose) {
        validateWebSocketClosePayload(payloadView);
    }
    offset = payloadOffset + payloadSize;
    pendingCompactUntil = offset;
    co_return WebSocketFrameView{
        .opcode = frameStart.opcode,
        .payload = payloadView,
        .fin = frameStart.fin,
        .continuation = frameStart.continuation,
        .rsv1 = frameStart.rsv1};
}

}  // namespace detail
}  // namespace ruvia
