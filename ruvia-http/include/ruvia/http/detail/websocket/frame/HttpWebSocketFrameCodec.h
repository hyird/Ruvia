#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocol.h"

// The RFC 6455 section 5.2 wire vocabulary: what a frame's first two bytes mean,
// which opcode and length combinations are legal, and the byte-level encode and
// unmask primitives. Everything here is a pure function of bytes -- no buffer
// ownership, no message state.

namespace ruvia::detail {

// The fixed-size scratch a frame header is encoded into (2 bytes plus up to an
// 8-byte extended length).
using WebSocketFrameHeader = std::array<char, 10>;

// Wire failures are protocol values, not exceptions. The numeric values are the
// RFC 6455 §7.4.1 Close codes the connection must send before ending transport.
enum class WebSocketProtocolFailure : std::uint16_t {
    kProtocolError = 1002,
    kInvalidPayloadData = 1007,
    kMessageTooLarge = 1009,
};

[[nodiscard]] constexpr std::uint16_t webSocketProtocolFailureCloseCode(WebSocketProtocolFailure failure) noexcept {
    return static_cast<std::uint16_t>(failure);
}

enum class WebSocketFrameKind : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

// Validated first-byte semantics. The raw continuation opcode is preserved as
// its own kind instead of being normalized to Text plus an independent boolean;
// compression is likewise admitted only where RFC 7692 permits RSV1.
class WebSocketFrameStart final {
public:
    [[nodiscard]] constexpr WebSocketFrameKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr bool final() const noexcept {
        return final_;
    }

    [[nodiscard]] constexpr bool compressed() const noexcept {
        return compressed_;
    }

private:
    friend std::optional<WebSocketFrameStart> decodeWebSocketFrameStart(unsigned char, unsigned char, bool) noexcept;
    friend std::optional<WebSocketFrameStart> decodeWebSocketFrameStart(unsigned char, unsigned char, bool, bool) noexcept;

    constexpr WebSocketFrameStart(WebSocketFrameKind kind, bool final, bool compressed) noexcept
        : kind_(kind),
          final_(final),
          compressed_(compressed) {}

    WebSocketFrameKind kind_;
    bool final_;
    bool compressed_;
};

[[nodiscard]] inline bool isInvalidWebSocketRawOpcode(std::uint8_t rawOpcode) noexcept {
    return (rawOpcode >= 0x3 && rawOpcode <= 0x7) || rawOpcode >= 0xB;
}

[[nodiscard]] inline bool isWebSocketControlOpcode(WebSocketOpcode opcode) noexcept {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

[[nodiscard]] inline bool isWebSocketControlFrameKind(WebSocketFrameKind kind) noexcept {
    return static_cast<std::uint8_t>(kind) >= 0x8;
}

// allowRsv1 enables the RSV1 (compressed) bit when permessage-deflate is
// negotiated; it is valid only on the first frame of a data message, never on a
// continuation or control frame (RFC 7692 §6.1). RSV2/RSV3 are always rejected.
[[nodiscard]] inline std::optional<WebSocketFrameStart> decodeWebSocketFrameStart(unsigned char first, unsigned char second, bool allowRsv1, bool expectMasked) noexcept {
    const auto rawOpcode = static_cast<std::uint8_t>(first & 0x0FU);
    const bool rsv1 = (first & 0x40U) != 0;
    if ((first & 0x30U) != 0 || ((second & 0x80U) != 0) != expectMasked || isInvalidWebSocketRawOpcode(rawOpcode)) {
        return std::nullopt;
    }
    if (rsv1 && (!allowRsv1 || rawOpcode == 0 || rawOpcode >= 0x8)) {
        return std::nullopt;
    }
    return WebSocketFrameStart(static_cast<WebSocketFrameKind>(rawOpcode), (first & 0x80U) != 0, rsv1);
}

[[nodiscard]] inline std::optional<WebSocketFrameStart> decodeWebSocketFrameStart(unsigned char first, unsigned char second, bool allowRsv1) noexcept {
    return decodeWebSocketFrameStart(first, second, allowRsv1, true);
}

[[nodiscard]] inline bool isInvalidWebSocketControlFrame(const WebSocketFrameStart& frame, std::uint64_t payloadSize) noexcept {
    return isWebSocketControlFrameKind(frame.kind()) && (!frame.final() || payloadSize > 125);
}

[[nodiscard]] inline bool webSocketMessageExceedsLimit(std::size_t payloadSize, ProtocolByteLimit messageLimit) noexcept {
    return messageLimit.exceeds(payloadSize);
}

[[nodiscard]] inline bool webSocketAppendExceedsLimit(std::size_t currentSize, std::size_t appendSize, ProtocolByteLimit messageLimit) noexcept {
    return messageLimit.additionExceeds(currentSize, appendSize);
}

[[nodiscard]] inline bool webSocketFrameLengthExceedsLimit(std::uint64_t payloadSize, ProtocolByteLimit messageLimit) noexcept {
    if (payloadSize > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return true;
    }
    return messageLimit.exceeds(static_cast<std::size_t>(payloadSize));
}

// A control frame (Close/Ping/Pong) is capped at 125 bytes by RFC 6455 §5.5 and
// is explicitly NOT subject to the per-message size limit, so only data frames
// (Text/Binary/Continuation) are measured against maxMessageBytes. Applying the
// limit to control frames would reject a legal Ping/Pong, or a Close carrying a
// reason phrase, once maxMessageBytes drops below 125 -- silently breaking the
// close handshake and keepalive on a small-message configuration.
[[nodiscard]] inline bool webSocketFrameExceedsMessageLimit(WebSocketFrameKind kind, std::uint64_t payloadSize, ProtocolByteLimit messageLimit) noexcept {
    return !isWebSocketControlFrameKind(kind) && webSocketFrameLengthExceedsLimit(payloadSize, messageLimit);
}

[[nodiscard]] inline bool webSocketMaskedFrameReadSizeOverflows(std::uint64_t payloadSize, std::size_t headerSize) noexcept {
    constexpr std::size_t kMaskBytes = 4;
    constexpr auto kMaxSize = (std::numeric_limits<std::size_t>::max)();
    return headerSize > kMaxSize - kMaskBytes || payloadSize > static_cast<std::uint64_t>(kMaxSize - headerSize - kMaskBytes);
}

[[nodiscard]] inline std::uint16_t readWebSocketUint16(const char* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<unsigned char>(data[0])) << 8) | static_cast<unsigned char>(data[1]));
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

[[nodiscard]] inline std::size_t encodeWebSocketFrameHeader(WebSocketFrameHeader& header, WebSocketOpcode opcode, std::size_t payloadSize, bool rsv1 = false, bool masked = false) noexcept {
    std::size_t headerSize = 0;
    header[headerSize++] = static_cast<char>(0x80U | (rsv1 ? 0x40U : 0U) | static_cast<std::uint8_t>(opcode));
    if (payloadSize <= 125) {
        header[headerSize++] = static_cast<char>((masked ? 0x80U : 0U) | payloadSize);
    } else if (payloadSize <= 0xFFFF) {
        header[headerSize++] = static_cast<char>((masked ? 0x80U : 0U) | 126U);
        header[headerSize++] = static_cast<char>((payloadSize >> 8) & 0xFF);
        header[headerSize++] = static_cast<char>(payloadSize & 0xFF);
    } else {
        header[headerSize++] = static_cast<char>((masked ? 0x80U : 0U) | 127U);
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
}  // namespace ruvia::detail
