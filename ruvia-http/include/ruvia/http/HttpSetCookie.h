#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>

namespace ruvia {

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes.
// Unknown attributes are ignored; malformed cookie-pairs are rejected.
struct HttpSetCookieView final {
    std::string_view name;
    std::string_view value;
    std::string_view path;
    std::string_view domain;
    std::optional<std::time_t> expires;
    std::optional<std::int64_t> maxAgeSeconds;
    bool secure{false};
};

[[nodiscard]] std::optional<HttpSetCookieView> parseSetCookie(std::string_view value) noexcept;

}  // namespace ruvia
