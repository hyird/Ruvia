#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are flags; delta-seconds directives are optional (absent = not present). This
// is a pure helper for building a caching reverse proxy / CDN edge on top of Context::proxy.
struct CacheControl {
    bool noStore{false};
    bool noCache{false};            // bare or field-name form -- both require revalidation
    bool mustRevalidate{false};
    bool proxyRevalidate{false};
    bool isPrivate{false};          // "private" (not for a shared cache)
    bool isPublic{false};
    bool immutable{false};
    std::optional<std::uint64_t> maxAge;
    std::optional<std::uint64_t> sMaxAge;               // shared-cache max age (wins for a proxy)
    std::optional<std::uint64_t> staleWhileRevalidate;
    std::optional<std::uint64_t> staleIfError;

    // Freshness lifetime a shared cache should use: s-maxage, else max-age, else none (the caller
    // falls back to Expires/heuristics). A response with no-store/no-cache is never served fresh.
    [[nodiscard]] std::optional<std::uint64_t> sharedFreshnessLifetime() const noexcept {
        if (sMaxAge) {
            return sMaxAge;
        }
        return maxAge;
    }
};

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] CacheControl parseCacheControl(std::string_view value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parseHttpDate(std::string_view value) noexcept;

}  // namespace ruvia
