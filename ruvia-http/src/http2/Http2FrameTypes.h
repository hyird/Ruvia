#pragma once

#include <cstdint>
#include <string_view>

#include "ruvia/http/Http2Framing.h"

namespace ruvia::detail {

using ruvia::Http2ErrorCode;
using ruvia::Http2FrameHeader;
using ruvia::Http2FrameType;
using ruvia::Http2SettingId;
using ruvia::kHttp2ClientPreface;
using ruvia::kHttp2DefaultMaxFrameSize;
using ruvia::kHttp2FrameHeaderBytes;

inline constexpr std::uint32_t kHttp2MaxFrameSizeLimit = kHttp2MaxFrameSize;
inline constexpr std::int32_t kHttp2DefaultInitialWindowSize = 65535;
inline constexpr std::uint8_t kHttp2FlagEndStream = 0x1;
inline constexpr std::uint8_t kHttp2FlagAck = 0x1;
inline constexpr std::uint8_t kHttp2FlagEndHeaders = 0x4;
inline constexpr std::uint8_t kHttp2FlagPadded = 0x8;
inline constexpr std::uint8_t kHttp2FlagPriority = 0x20;

}  // namespace ruvia::detail
