#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

inline constexpr std::string_view kHttp2ClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
inline constexpr std::size_t kHttp2FrameHeaderBytes = 9;
inline constexpr std::uint32_t kHttp2DefaultMaxFrameSize = 16 * 1024;
inline constexpr std::uint32_t kHttp2MaxFrameSizeLimit = 16 * 1024 * 1024 - 1;
inline constexpr std::int32_t kHttp2DefaultInitialWindowSize = 65535;
inline constexpr std::uint8_t kHttp2FlagEndStream = 0x1;
inline constexpr std::uint8_t kHttp2FlagAck = 0x1;
inline constexpr std::uint8_t kHttp2FlagEndHeaders = 0x4;
inline constexpr std::uint8_t kHttp2FlagPadded = 0x8;
inline constexpr std::uint8_t kHttp2FlagPriority = 0x20;

enum class Http2FrameType : std::uint8_t {
    kData = 0x0,
    kHeaders = 0x1,
    kPriority = 0x2,
    kRstStream = 0x3,
    kSettings = 0x4,
    kPushPromise = 0x5,
    kPing = 0x6,
    kGoaway = 0x7,
    kWindowUpdate = 0x8,
    kContinuation = 0x9,
};

// RFC 7540 serializes error codes as four-octet unsigned integers.
// NOLINTNEXTLINE(performance-enum-size)
enum class Http2ErrorCode : std::uint32_t {
    kNoError = 0x0,
    kProtocolError = 0x1,
    kInternalError = 0x2,
    kFlowControlError = 0x3,
    kSettingsTimeout = 0x4,
    kStreamClosed = 0x5,
    kFrameSizeError = 0x6,
    kRefusedStream = 0x7,
    kCancel = 0x8,
    kCompressionError = 0x9,
    kConnectError = 0xa,
    kEnhanceYourCalm = 0xb,
    kInadequateSecurity = 0xc,
    kHttp11Required = 0xd,
};

// HTTP/2 SETTINGS identifiers occupy two octets on the wire.
// NOLINTNEXTLINE(performance-enum-size)
enum class Http2SettingId : std::uint16_t {
    kHeaderTableSize = 0x1,
    kEnablePush = 0x2,
    kMaxConcurrentStreams = 0x3,
    kInitialWindowSize = 0x4,
    kMaxFrameSize = 0x5,
    kMaxHeaderListSize = 0x6,
    kEnableConnectProtocol = 0x8,
};

struct Http2FrameHeader final {
    std::uint32_t length{0};
    std::uint8_t type{0};
    std::uint8_t flags{0};
    std::uint32_t streamId{0};
};

}  // namespace ruvia::detail
