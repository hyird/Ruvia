#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/edge/detail/cache/EdgeCache.h"
#include "ruvia/edge/detail/proxy/HeaderRules.h"

namespace ruvia::edge {

// What a cached entry turns into when the client asked for a byte range: the
// requested slice as 206, or 416 when the range lies outside the body. The
// headers are the entry's own plus the Content-Range the answer must carry.
struct CachedRangeResponse final {
    std::uint16_t status{0};
    Headers headers;
    std::string_view body;
    // A 416 describes no representation, so it carries no Age.
    bool withAge{false};
};

// Apply a Range header to a cached body. Returns nullopt when the header asks
// for nothing this edge honours (a multi-range, another unit, a malformed spec),
// which RFC 9110 section 14.2 says to ignore and serve in full.
//
// The returned body borrows `entry`, which must outlive the response.
[[nodiscard]] std::optional<CachedRangeResponse> cachedRangeResponse(const CachedResponse& entry, std::string_view rangeHeader);

}  // namespace ruvia::edge
