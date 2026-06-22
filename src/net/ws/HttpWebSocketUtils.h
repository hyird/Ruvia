#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
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
};

[[nodiscard]] inline bool isInvalidWebSocketRawOpcode(std::uint8_t rawOpcode) noexcept {
    return (rawOpcode >= 0x3 && rawOpcode <= 0x7) || rawOpcode >= 0xB;
}

[[nodiscard]] inline bool isWebSocketControlOpcode(WebSocketOpcode opcode) noexcept {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

[[nodiscard]] inline bool decodeWebSocketFrameStart(
    unsigned char first,
    unsigned char second,
    WebSocketFrameStart& frame) noexcept {
    const auto rawOpcode = static_cast<std::uint8_t>(first & 0x0FU);
    if ((first & 0x70U) != 0 || (second & 0x80U) == 0 || isInvalidWebSocketRawOpcode(rawOpcode)) {
        return false;
    }
    frame = WebSocketFrameStart{
        .opcode = static_cast<WebSocketOpcode>(rawOpcode == 0 ? 0x1 : rawOpcode),
        .fin = (first & 0x80U) != 0,
        .continuation = rawOpcode == 0};
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
    std::size_t payloadSize) noexcept {
    std::size_t headerSize = 0;
    header[headerSize++] = static_cast<char>(0x80U | static_cast<std::uint8_t>(opcode));
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

}  // namespace detail
}  // namespace ruvia
