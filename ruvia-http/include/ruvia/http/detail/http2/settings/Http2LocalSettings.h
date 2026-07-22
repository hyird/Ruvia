#pragma once

#include <cstddef>
#include <cstdint>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

// One compile-time source of truth for every local receive constraint. These values
// drive both the advertised SETTINGS bytes and the matching in-memory accounting /
// fixed capacities; they are intentionally not runtime knobs that can diverge.
struct Http2LocalSettings final {
    static constexpr std::uint32_t kHeaderTableSize = 4096;
    static constexpr std::uint32_t kEnablePush = 0;
    static constexpr std::uint32_t kMaxConcurrentStreams = 128;
    static constexpr std::uint32_t kInitialWindowSize = 1024 * 1024;
    static constexpr std::uint32_t kMaxFrameSize = kHttp2DefaultMaxFrameSize;
    static constexpr std::uint32_t kMaxHeaderListSize =
        static_cast<std::uint32_t>(kMaxHttpHeaderBytes);
    static constexpr std::uint32_t kEnableConnectProtocol = 1;
    static constexpr std::uint32_t kEntryCount = 7;
    static constexpr std::uint32_t kPayloadBytes = kEntryCount * 6;
    static constexpr std::size_t kFrameBytes =
        kHttp2FrameHeaderBytes + kPayloadBytes;
};

static_assert(Http2LocalSettings::kEnablePush <= 1);
static_assert(Http2LocalSettings::kEnableConnectProtocol <= 1);
static_assert(Http2LocalSettings::kInitialWindowSize <= 0x7fffffffU);
static_assert(Http2LocalSettings::kMaxFrameSize >= kHttp2DefaultMaxFrameSize);
static_assert(Http2LocalSettings::kMaxFrameSize <= kHttp2MaxFrameSizeLimit);

inline char* http2WriteLocalSettingsFrame(char* out) noexcept {
    out = http2WriteFrameHeader(
        out, Http2LocalSettings::kPayloadBytes, Http2FrameType::kSettings, 0, 0);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kHeaderTableSize, Http2LocalSettings::kHeaderTableSize);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kEnablePush, Http2LocalSettings::kEnablePush);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kMaxConcurrentStreams,
        Http2LocalSettings::kMaxConcurrentStreams);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kInitialWindowSize,
        Http2LocalSettings::kInitialWindowSize);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kMaxFrameSize, Http2LocalSettings::kMaxFrameSize);
    out = http2WriteSettingsEntry(
        out, Http2SettingId::kMaxHeaderListSize,
        Http2LocalSettings::kMaxHeaderListSize);
    return http2WriteSettingsEntry(
        out, Http2SettingId::kEnableConnectProtocol,
        Http2LocalSettings::kEnableConnectProtocol);
}

}  // namespace ruvia::detail
