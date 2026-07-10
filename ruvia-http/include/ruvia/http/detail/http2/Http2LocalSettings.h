#pragma once

#include <cstddef>
#include <cstdint>

#include "ruvia/http/detail/http2/Http2Frame.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

inline constexpr std::uint32_t kHttp2LocalSettingsPayloadBytes = 7 * 6;
inline constexpr std::size_t kHttp2LocalSettingsFrameBytes =
    kHttp2FrameHeaderBytes + kHttp2LocalSettingsPayloadBytes;

inline char* http2WriteLocalSettingsFrame(char* out) noexcept {
    out = http2WriteFrameHeader(out, kHttp2LocalSettingsPayloadBytes, Http2FrameType::kSettings, 0, 0);
    out = http2WriteSettingsEntry(out, Http2SettingId::kHeaderTableSize, 4096);
    out = http2WriteSettingsEntry(out, Http2SettingId::kEnablePush, 0);
    out = http2WriteSettingsEntry(out, Http2SettingId::kMaxConcurrentStreams, kHttp2LocalMaxConcurrentStreams);
    out = http2WriteSettingsEntry(out, Http2SettingId::kInitialWindowSize, kHttp2LocalInitialWindowSize);
    out = http2WriteSettingsEntry(out, Http2SettingId::kMaxFrameSize, kHttp2DefaultMaxFrameSize);
    out = http2WriteSettingsEntry(out, Http2SettingId::kMaxHeaderListSize, kMaxHttpHeaderBytes);
    return http2WriteSettingsEntry(out, Http2SettingId::kEnableConnectProtocol, 1);
}

}  // namespace ruvia::detail
