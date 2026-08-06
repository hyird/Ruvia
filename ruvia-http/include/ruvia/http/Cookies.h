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

struct CookieOptions final {
    // Cookie attributes are retained by SetCookiePlan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::BorrowedText path{"/"};
    ::ruvia::BorrowedText domain;
    std::optional<CookieSameSite> sameSite;
    std::optional<CookiePriority> priority;
    std::optional<std::chrono::system_clock::time_point> expires;
    std::optional<std::chrono::seconds> maxAge;
    std::optional<CookiePrefix> prefix;
    bool httpOnly{false};
    bool secure{false};
    bool partitioned{false};
};

}  // namespace ruvia
