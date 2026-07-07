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

struct CookieOptions {
    std::string_view path{"/"};
    std::string_view domain;
    std::string_view sameSite;
    std::string_view priority;  // "Low" | "Medium" | "High"
    std::optional<std::chrono::system_clock::time_point> expires;
    std::int64_t maxAge{-1};
    CookiePrefix prefix{CookiePrefix::kNone};
    bool httpOnly{false};
    bool secure{false};
    bool partitioned{false};
};

}  // namespace ruvia
