#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include "Http2Frame.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2WindowUpdateFrameBytes = kHttp2FrameHeaderBytes + 4;

inline void http2AppendDataWindowUpdates(
    std::pmr::string& out,
    std::uint32_t streamId,
    std::uint32_t increment) {
    if (increment == 0) {
        return;
    }
    out.reserve(kHttp2WindowUpdateFrameBytes * 2);
    http2AppendWindowUpdate(out, 0, increment);
    http2AppendWindowUpdate(out, streamId, increment);
}

}  // namespace ruvia::detail
