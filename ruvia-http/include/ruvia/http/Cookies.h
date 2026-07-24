#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

enum class CookiePrefix : std::uint8_t {
    kSecure,  // serializes the name as "__Secure-<name>"; requires secure
    kHost,    // serializes the name as "__Host-<name>"; requires secure, Path=/, no Domain
};

enum class CookieSameSite : std::uint8_t {
    kStrict,
    kLax,
    kNone,  // the literal SameSite=None attribute; requires Secure
};

enum class CookiePriority : std::uint8_t {
    kLow,
    kMedium,
    kHigh,
};

struct CookieOptions final {
    // Cookie attributes are retained by SetCookiePlan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    class BorrowedText final {
    public:
        constexpr BorrowedText() noexcept = default;

        constexpr BorrowedText(std::string_view value) noexcept
            : value_(value) {}

        constexpr BorrowedText(const char* value) noexcept
            : value_(value) {}

        template <typename Traits, typename Allocator>
        constexpr BorrowedText(const std::basic_string<char, Traits, Allocator>& value) noexcept
            : value_(value) {}

        template <detail::HttpTemporaryOwningCharString String>
        BorrowedText(String&&) = delete;

        constexpr BorrowedText& operator=(std::string_view value) noexcept {
            value_ = value;
            return *this;
        }

        constexpr BorrowedText& operator=(const char* value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <typename Traits, typename Allocator>
        constexpr BorrowedText& operator=(const std::basic_string<char, Traits, Allocator>& value) noexcept {
            value_ = std::string_view(value);
            return *this;
        }

        template <detail::HttpTemporaryOwningCharString String>
        BorrowedText& operator=(String&&) = delete;

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr operator std::string_view() const noexcept {
            return value_;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return value_.empty();
        }

        friend constexpr bool operator==(BorrowedText left, BorrowedText right) noexcept {
            return left.value_ == right.value_;
        }

        friend constexpr bool operator==(BorrowedText left, std::string_view right) noexcept {
            return left.value_ == right;
        }

        friend constexpr bool operator==(BorrowedText left, const char* right) noexcept {
            return left.value_ == right;
        }

    private:
        std::string_view value_;
    };

    BorrowedText path{"/"};
    BorrowedText domain;
    std::optional<CookieSameSite> sameSite;
    std::optional<CookiePriority> priority;
    std::optional<std::chrono::system_clock::time_point> expires;
    std::optional<std::chrono::seconds> maxAge;
    std::optional<CookiePrefix> prefix;
    bool httpOnly{false};
    bool secure{false};
    bool partitioned{false};
};

static_assert(sizeof(CookieOptions::BorrowedText) == sizeof(std::string_view));

}  // namespace ruvia
