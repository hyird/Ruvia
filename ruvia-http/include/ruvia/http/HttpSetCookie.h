#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

enum class HttpSetCookieAttribute : std::uint8_t {
    kSecure = 1U << 0,
    kPath = 1U << 1,
    kSameSiteNone = 1U << 2,
};

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes.
// Unknown and oversized attributes are ignored; invalid received cookies are
// rejected.
class HttpSetCookieView final {
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

    [[nodiscard]] constexpr std::optional<std::int64_t> maxAgeSeconds() const noexcept {
        return maxAgeSeconds_;
    }

    [[nodiscard]] constexpr bool has(HttpSetCookieAttribute attribute) const noexcept {
        const auto mask = static_cast<std::uint8_t>(attribute);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (attributes_ & mask) != 0U;
    }

private:
    friend std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept;

    constexpr HttpSetCookieView(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    constexpr void set(HttpSetCookieAttribute attribute) noexcept {
        attributes_ |= static_cast<std::uint8_t>(attribute);
    }

    constexpr void clear(HttpSetCookieAttribute attribute) noexcept {
        attributes_ &= static_cast<std::uint8_t>(~static_cast<std::uint8_t>(attribute));
    }

    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::time_t> expires_;
    std::optional<std::int64_t> maxAgeSeconds_;
    std::uint8_t attributes_{0};
};

[[nodiscard]] std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept;

template <detail::HttpTemporaryOwningCharString Value>
std::optional<HttpSetCookieView> parseSetCookie(Value&&) = delete;

}  // namespace ruvia
