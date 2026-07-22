#pragma once

#include <span>

#include "ruvia/edge/detail/EdgeHeaderRules.h"
#include "ruvia/http/HttpCache.h"

// What a client request says about cache use, read once per request: its
// Cache-Control (plus the legacy Pragma: no-cache a request without
// Cache-Control may still carry), whether it is conditional, and whether it
// carries credentials. These decide whether a stored response may be used at
// all, before any lookup happens.

namespace ruvia::edge {

struct EdgeRequestDirectives final {
    CacheControl cacheControl;
    bool hasAuthorization{false};
    bool hasCondition{false};
    // The request demands the origin be consulted: no-cache, a max-age or
    // min-fresh constraint the edge does not evaluate itself, or legacy Pragma.
    bool forcesValidation{false};
};

[[nodiscard]] EdgeRequestDirectives edgeRequestDirectives(
    std::span<const HttpHeaderView> headers) noexcept;

}  // namespace ruvia::edge
