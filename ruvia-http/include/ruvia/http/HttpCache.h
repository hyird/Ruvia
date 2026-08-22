#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

enum class CacheControlDirective : std::uint16_t {
    kNoStore = 1U << 0,
    kNoCache = 1U << 1,
    kNoTransform = 1U << 2,
    kMustRevalidate = 1U << 3,
    kProxyRevalidate = 1U << 4,
    kPrivate = 1U << 5,
    kPublic = 1U << 6,
    kImmutable = 1U << 7,
    kOnlyIfCached = 1U << 8,
    kMaxStaleAny = 1U << 9,
};

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are queried by directive enum; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
class CacheControl final {
public:
    [[nodiscard]] constexpr bool has(CacheControlDirective directive) const noexcept {
        const auto mask = static_cast<std::uint16_t>(directive);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (directives_ & mask) != 0U;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> maxAge() const noexcept {
        return maxAge_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> maxStale() const noexcept {
        return maxStale_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> minFresh() const noexcept {
        return minFresh_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> sMaxAge() const noexcept {
        return sMaxAge_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> staleWhileRevalidate() const noexcept {
        return staleWhileRevalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> staleIfError() const noexcept {
        return staleIfError_;
    }

private:
    friend class CacheControlFieldParser;

    constexpr void set(CacheControlDirective directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> maxAge_;
    std::optional<std::uint64_t> maxStale_;
    std::optional<std::uint64_t> minFresh_;
    std::optional<std::uint64_t> sMaxAge_;
    std::optional<std::uint64_t> staleWhileRevalidate_;
    std::optional<std::uint64_t> staleIfError_;
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
    bool maxStaleSeen_{false};
    bool minFreshSeen_{false};
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
