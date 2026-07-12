#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ruvia {

enum class CookiePrefix : std::uint8_t {
    kNone,
    kSecure,  // serializes the name as "__Secure-<name>"; requires secure
    kHost,    // serializes the name as "__Host-<name>"; requires secure, Path=/, no Domain
};

enum class CookieSameSite : std::uint8_t {
    kUnspecified,
    kStrict,
    kLax,
    kNone,
};

enum class CookiePriority : std::uint8_t {
    kUnspecified,
    kLow,
    kMedium,
    kHigh,
};

struct CookieOptions final {
    std::string_view path{"/"};
    std::string_view domain;
    CookieSameSite sameSite{CookieSameSite::kUnspecified};
    CookiePriority priority{CookiePriority::kUnspecified};
    std::optional<std::chrono::system_clock::time_point> expires;
    std::optional<std::chrono::seconds> maxAge;
    CookiePrefix prefix{CookiePrefix::kNone};
    bool httpOnly{false};
    bool secure{false};
    bool partitioned{false};
};

}  // namespace ruvia
