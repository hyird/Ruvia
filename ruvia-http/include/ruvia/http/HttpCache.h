#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are flags; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
struct CacheControl {
    bool noStore{false};
    bool noCache{false};            // bare or field-name form -- both require revalidation
    bool noTransform{false};        // intermediaries must not transform the content
    bool mustRevalidate{false};
    bool proxyRevalidate{false};
    bool isPrivate{false};          // "private" (not for a shared cache)
    bool isPublic{false};
    bool immutable{false};
    std::optional<std::uint64_t> maxAge;
    std::optional<std::uint64_t> sMaxAge;
    std::optional<std::uint64_t> staleWhileRevalidate;
    std::optional<std::uint64_t> staleIfError;
};

// Incrementally parses every Cache-Control field line as one logical directive
// list (RFC 9110 section 5.2). State spans updates so duplicate freshness
// directives keep the first occurrence even when they appear on different lines.
class CacheControlFieldParser final {
public:
    void update(std::string_view fieldValue) noexcept;

    [[nodiscard]] CacheControl finish() const noexcept {
        return value_;
    }

private:
    CacheControl value_;
    bool maxAgeSeen_{false};
    bool sMaxAgeSeen_{false};
    bool staleWhileRevalidateSeen_{false};
    bool staleIfErrorSeen_{false};
};

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] CacheControl parseCacheControl(std::string_view value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parseHttpDate(std::string_view value) noexcept;

}  // namespace ruvia
