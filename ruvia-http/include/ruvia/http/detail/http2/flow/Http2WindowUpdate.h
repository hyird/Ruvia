#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2WindowUpdateFrameBytes = kHttp2FrameHeaderBytes + 4;

enum class Http2WindowUpdateResult : std::uint8_t { kOk, kZeroIncrement, kOverflow };

[[nodiscard]] inline std::uint32_t http2WindowUpdateIncrement(std::string_view payload) noexcept {
    return http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
}

[[nodiscard]] inline Http2WindowUpdateResult http2ApplyWindowUpdate(
    std::int32_t& window, std::uint32_t increment) noexcept {
    if (increment == 0) {
        return Http2WindowUpdateResult::kZeroIncrement;
    }
    const auto amount = static_cast<std::int32_t>(increment);
    if (window > std::numeric_limits<std::int32_t>::max() - amount) {
        return Http2WindowUpdateResult::kOverflow;
    }
    window += amount;
    return Http2WindowUpdateResult::kOk;
}

}  // namespace ruvia::detail
