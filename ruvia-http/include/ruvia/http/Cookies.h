#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/BorrowedText.h"

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

enum class CookieAttributePolicy : std::uint8_t {
    kOmit,
    kEmit,
};

// RFC 6265bis: cookie lifetimes SHOULD NOT exceed 400 days.
inline constexpr std::int64_t kMaxCookieAgeSeconds = 34560000;

// Cookie request pairs always include '=', including for an empty name.
template <typename String>
void appendCookieRequestPair(String& header, std::string_view name, std::string_view value) {
    if (!header.empty()) {
        header.append("; ", 2);
    }
    header.append(name.data(), name.size());
    header.push_back('=');
    header.append(value.data(), value.size());
}

// Cookie octet validation and name-prefix parsing are protocol rules shared by
// the Set-Cookie writer and outbound cookie jar.
[[nodiscard]] bool isValidCookieValue(std::string_view value) noexcept;
[[nodiscard]] bool cookieNameStartsWithIgnoreCase(
    std::string_view name, std::string_view prefix) noexcept;
[[nodiscard]] std::string_view httpCookiePrefixText(CookiePrefix prefix) noexcept;

struct CookieOptions final {
    // Cookie attributes are retained by SetCookiePlan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::BorrowedText path{"/"};
    ::ruvia::BorrowedText domain{};
    std::optional<CookieSameSite> sameSite{};
    std::optional<CookiePriority> priority{};
    // UTC, rounded down to whole seconds; year >= 1601 and at most 400 days ahead.
    std::optional<std::chrono::system_clock::time_point> expires{};
    std::optional<std::chrono::seconds> maxAge{};
    std::optional<CookiePrefix> prefix{};
    CookieAttributePolicy httpOnly{CookieAttributePolicy::kOmit};
    CookieAttributePolicy secure{CookieAttributePolicy::kOmit};
    CookieAttributePolicy partitioned{CookieAttributePolicy::kOmit};
};

}  // namespace ruvia
