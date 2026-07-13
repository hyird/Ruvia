#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

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
    std::string_view path{"/"};
    std::string_view domain;
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
