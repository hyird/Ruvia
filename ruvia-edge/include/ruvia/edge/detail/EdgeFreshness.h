#pragma once

#include <cstdint>
#include <ctime>
#include <optional>

#include "ruvia/edge/detail/EdgeHeaderRules.h"
#include "ruvia/http/HttpCache.h"

namespace ruvia::edge {

// Internal input describing what the shared cache needs to know to decide
// whether, and for how long, it may be reused. This is the RFC 9111 freshness
// input assembled from the response's status line and its Cache-Control, Date,
// Expires and Age header fields; the caller parses those, this applies policy.
struct FreshnessInput final {
    int status{0};
    ruvia::CacheControl cacheControl;             // parsed response Cache-Control
    bool requestHasAuthorization{false};          // shared-cache storage gate
    std::optional<std::time_t> dateHeader;        // origin Date, if present and valid
    std::optional<std::time_t> expiresHeader;     // Expires, if present and valid
    std::uint64_t ageHeader{0};                   // Age in seconds (0 if absent)
    std::time_t requestTime{0};                   // upstream request start
    std::time_t now{0};                           // response reception time
};

// The caching decision. When cacheable is false the other fields are unset and
// the response must be forwarded without storing. When true, expiresAt is the
// absolute instant the stored response turns stale, and the stale-* windows say
// how long past that a stale copy may still be served (while revalidating, or
// when the origin later errors) -- the reuse policy the cache lookup enforces.
struct FreshnessDecision final {
    bool cacheable{false};
    std::time_t expiresAt{0};
    std::uint64_t initialAge{0};
    std::uint64_t staleWhileRevalidate{0};
    std::uint64_t staleIfError{0};
};

// Decide whether an origin response may be stored in a shared (edge) cache and
// compute its freshness deadline. Conservative by design: a response is stored
// only when it carries an explicit freshness signal (s-maxage, max-age, or a
// usable Expires) -- no heuristic freshness -- and only for status codes that
// are safe to reuse. no-store and private responses are never stored, and a
// response that is already stale on arrival (its age exceeds its lifetime) is
// rejected rather than stored dead. s-maxage takes precedence over max-age
// because this is a shared cache.
[[nodiscard]] FreshnessDecision evaluateFreshness(const FreshnessInput& input) noexcept;

// Assemble the RFC 9111 freshness inputs from a response's status and headers.
[[nodiscard]] FreshnessInput buildFreshnessInput(
    std::uint16_t status,
    const Headers& headers,
    std::time_t now,
    std::time_t requestTime,
    bool requestHasAuthorization);

}  // namespace ruvia::edge
