#pragma once

#include <cstddef>
#include <cstdint>

#include "Http2Frame.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2WindowUpdateFrameBytes = kHttp2FrameHeaderBytes + 4;

inline char* http2WriteDataWindowUpdates(
    char* out,
    std::uint32_t streamId,
    std::uint32_t increment) noexcept {
    if (increment == 0) {
        return out;
    }
    out = http2WriteWindowUpdate(out, 0, increment);
    return http2WriteWindowUpdate(out, streamId, increment);
}

}  // namespace ruvia::detail
