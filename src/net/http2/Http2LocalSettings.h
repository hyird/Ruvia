#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include "Http2Frame.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

inline constexpr std::uint32_t kHttp2LocalSettingsPayloadBytes = 7 * 6;
inline constexpr std::size_t kHttp2LocalSettingsFrameBytes =
    kHttp2FrameHeaderBytes + kHttp2LocalSettingsPayloadBytes;

inline void http2AppendLocalSettingsFrame(std::pmr::string& out) {
    http2AppendFrameHeader(out, kHttp2LocalSettingsPayloadBytes, Http2FrameType::kSettings, 0, 0);
    http2AppendSettingsEntry(out, Http2SettingId::kHeaderTableSize, 4096);
    http2AppendSettingsEntry(out, Http2SettingId::kEnablePush, 0);
    http2AppendSettingsEntry(out, Http2SettingId::kMaxConcurrentStreams, kHttp2LocalMaxConcurrentStreams);
    http2AppendSettingsEntry(out, Http2SettingId::kInitialWindowSize, kHttp2LocalInitialWindowSize);
    http2AppendSettingsEntry(out, Http2SettingId::kMaxFrameSize, kHttp2DefaultMaxFrameSize);
    http2AppendSettingsEntry(out, Http2SettingId::kMaxHeaderListSize, kMaxHttpHeaderBytes);
    http2AppendSettingsEntry(out, Http2SettingId::kEnableConnectProtocol, 1);
}

}  // namespace ruvia::detail
