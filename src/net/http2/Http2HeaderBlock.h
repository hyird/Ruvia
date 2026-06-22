#pragma once

#include <string_view>

#include "Http2StreamState.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

inline void http2ResetHeaderBlock(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.headerBlock);
}

[[nodiscard]] inline bool http2AppendHeaderBlock(Http2StreamState& stream, std::string_view fragment) {
    const auto current = stream.headerBlock.size();
    if (current > kMaxHttpHeaderBytes || fragment.size() > kMaxHttpHeaderBytes - current) {
        return false;
    }
    stream.headerBlock.append(fragment.data(), fragment.size());
    return true;
}

[[nodiscard]] inline bool http2StartHeaderBlock(Http2StreamState& stream, std::string_view fragment) {
    http2ResetHeaderBlock(stream);
    return http2AppendHeaderBlock(stream, fragment);
}

}  // namespace ruvia::detail
