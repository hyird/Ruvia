#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/util/PmrString.h"

namespace ruvia::detail {

// SETTINGS_MAX_HEADER_LIST_SIZE limits the decoded field section, not the
// serialized HPACK block. An HPACK Huffman code can consume up to 30 bits per
// decoded octet, so a valid block can be larger than kMaxHttpHeaderBytes. Four
// encoded bytes per decoded-budget byte cover that expansion plus the bounded
// representation overhead while retaining a hard memory limit for incomplete
// CONTINUATION sequences.
inline constexpr std::size_t kMaxHttp2EncodedHeaderBlockBytes = 4 * kMaxHttpHeaderBytes;

inline void http2ResetHeaderBlock(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.remoteHeaderBlock());
}

[[nodiscard]] inline bool http2AppendHeaderBlock(Http2StreamState& stream, std::string_view fragment) {
    auto& headerBlock = stream.remoteHeaderBlock();
    const auto current = headerBlock.size();
    if (current > kMaxHttp2EncodedHeaderBlockBytes || fragment.size() > kMaxHttp2EncodedHeaderBlockBytes - current) {
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
