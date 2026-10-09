#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/util/borrowed_view.h"

namespace ruvia {

enum class http_set_cookie_attribute : std::uint8_t {
    secure = 1U << 0,
    path = 1U << 1,
    same_site_none = 1U << 2,
};

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes.
// Unknown and oversized attributes are ignored; invalid received cookies are
// rejected. Expires follows RFC 6265 cookie-date token grammar, including its
// permitted suffixes; this does not relax the separate HTTP-date grammar.
class http_set_cookie_view final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr std::string_view domain() const noexcept {
        return domain_;
    }

    [[nodiscard]] constexpr std::optional<std::time_t> expires() const noexcept {
        return expires_;
    }

    [[nodiscard]] constexpr std::optional<std::int64_t> max_age_seconds() const noexcept {
        return max_age_seconds_;
    }

    [[nodiscard]] constexpr bool has(http_set_cookie_attribute attribute) const noexcept {
        const auto mask = static_cast<std::uint8_t>(attribute);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (attributes_ & mask) != 0U;
    }

private:
    friend std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

    constexpr http_set_cookie_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    constexpr void set(http_set_cookie_attribute attribute) noexcept {
        attributes_ |= static_cast<std::uint8_t>(attribute);
    }

    constexpr void clear(http_set_cookie_attribute attribute) noexcept {
        attributes_ &= static_cast<std::uint8_t>(~static_cast<std::uint8_t>(attribute));
    }

    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::time_t> expires_;
    std::optional<std::int64_t> max_age_seconds_;
    std::uint8_t attributes_{0};
};

[[nodiscard]] std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

template <detail::http_temporary_owning_char_string value_type>
std::optional<http_set_cookie_view> parse_set_cookie(value_type&&) = delete;

}  // namespace ruvia
