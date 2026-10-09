#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_value = 1U << 5,
    public_value = 1U << 6,
    immutable = 1U << 7,
    only_if_cached = 1U << 8,
    max_stale_any = 1U << 9,
};

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are queried by directive enum; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
class cache_control final {
public:
    [[nodiscard]] constexpr bool has(cache_control_directive directive) const noexcept {
        const auto mask = static_cast<std::uint16_t>(directive);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (directives_ & mask) != 0U;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_age() const noexcept {
        return max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_stale() const noexcept {
        return max_stale_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> min_fresh() const noexcept {
        return min_fresh_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_max_age() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept {
        return stale_while_revalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept {
        return stale_if_error_;
    }

private:
    friend class cache_control_field_parser;

    constexpr void set(cache_control_directive directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> max_age_;
    std::optional<std::uint64_t> max_stale_;
    std::optional<std::uint64_t> min_fresh_;
    std::optional<std::uint64_t> s_max_age_;
    std::optional<std::uint64_t> stale_while_revalidate_;
    std::optional<std::uint64_t> stale_if_error_;
};

// Incrementally parses every Cache-Control field line as one logical directive
// list (RFC 9110 section 5.2). State spans updates so duplicate freshness
// directives keep the first occurrence even when they appear on different lines.
class cache_control_field_parser final {
public:
    void update(std::string_view field_value) noexcept;

    [[nodiscard]] cache_control finish() const noexcept {
        return value_;
    }

private:
    cache_control value_;
    bool max_age_seen_{false};
    bool max_stale_seen_{false};
    bool min_fresh_seen_{false};
    bool s_max_age_seen_{false};
    bool stale_while_revalidate_seen_{false};
    bool stale_if_error_seen_{false};
};

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;

}  // namespace ruvia
