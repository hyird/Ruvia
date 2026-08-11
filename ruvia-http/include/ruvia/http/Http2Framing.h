#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ruvia {

inline constexpr std::string_view kHttp2ClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t kHttp2FrameHeaderBytes = 9;
inline constexpr std::uint32_t kHttp2DefaultMaxFrameSize = 16 * 1024;
inline constexpr std::uint32_t kHttp2MaxFrameSize = 16 * 1024 * 1024 - 1;

enum class Http2FrameType : std::uint8_t { kData = 0x0, kHeaders = 0x1, kPriority = 0x2, kRstStream = 0x3, kSettings = 0x4, kPushPromise = 0x5, kPing = 0x6, kGoaway = 0x7, kWindowUpdate = 0x8, kContinuation = 0x9 };
enum class Http2ErrorCode : std::uint32_t { kNoError = 0x0, kProtocolError = 0x1, kInternalError = 0x2, kFlowControlError = 0x3, kSettingsTimeout = 0x4, kStreamClosed = 0x5, kFrameSizeError = 0x6, kRefusedStream = 0x7, kCancel = 0x8, kCompressionError = 0x9, kConnectError = 0xa, kEnhanceYourCalm = 0xb, kInadequateSecurity = 0xc, kHttp11Required = 0xd };
enum class Http2SettingId : std::uint16_t { kHeaderTableSize = 0x1, kEnablePush = 0x2, kMaxConcurrentStreams = 0x3, kInitialWindowSize = 0x4, kMaxFrameSize = 0x5, kMaxHeaderListSize = 0x6, kEnableConnectProtocol = 0x8 };

struct Http2FrameHeader final {
    std::uint32_t length{0};
    std::uint8_t type{0};
    std::uint8_t flags{0};
    std::uint32_t streamId{0};
};

[[nodiscard]] inline std::optional<Http2FrameHeader> parseHttp2FrameHeader(std::span<const char> bytes) noexcept {
    if (bytes.size() < kHttp2FrameHeaderBytes) return std::nullopt;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    return Http2FrameHeader{
        .length = (static_cast<std::uint32_t>(data[0]) << 16) | (static_cast<std::uint32_t>(data[1]) << 8) | data[2],
        .type = data[3],
        .flags = data[4],
        .streamId = (static_cast<std::uint32_t>(data[5] & 0x7f) << 24) | (static_cast<std::uint32_t>(data[6]) << 16) | (static_cast<std::uint32_t>(data[7]) << 8) | data[8],
    };
}

[[nodiscard]] inline bool encodeHttp2FrameHeader(std::span<char> output, std::uint32_t length, Http2FrameType type, std::uint8_t flags, std::uint32_t streamId) noexcept {
    if (output.size() < kHttp2FrameHeaderBytes || length > kHttp2MaxFrameSize || streamId > 0x7fffffffU) return false;
    output[0] = static_cast<char>((length >> 16) & 0xff);
    output[1] = static_cast<char>((length >> 8) & 0xff);
    output[2] = static_cast<char>(length & 0xff);
    output[3] = static_cast<char>(type);
    output[4] = static_cast<char>(flags);
    output[5] = static_cast<char>((streamId >> 24) & 0x7f);
    output[6] = static_cast<char>((streamId >> 16) & 0xff);
    output[7] = static_cast<char>((streamId >> 8) & 0xff);
    output[8] = static_cast<char>(streamId & 0xff);
    return true;
}

}  // namespace ruvia
