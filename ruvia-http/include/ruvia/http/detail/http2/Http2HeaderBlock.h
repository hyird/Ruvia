#pragma once

#include <string_view>

#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

inline void http2ResetHeaderBlock(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.requestHeaderBlock());
}

[[nodiscard]] inline bool http2AppendHeaderBlock(Http2StreamState& stream, std::string_view fragment) {
    auto& headerBlock = stream.requestHeaderBlock();
    const auto current = headerBlock.size();
    if (current > kMaxHttpHeaderBytes || fragment.size() > kMaxHttpHeaderBytes - current) {
        return false;
    }
    headerBlock.append(fragment.data(), fragment.size());
    return true;
}

[[nodiscard]] inline bool http2StartHeaderBlock(Http2StreamState& stream, std::string_view fragment) {
    http2ResetHeaderBlock(stream);
    return http2AppendHeaderBlock(stream, fragment);
}

}  // namespace ruvia::detail
