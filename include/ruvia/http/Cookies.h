#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

struct CookieOptions {
    std::string_view path{"/"};
    std::string_view domain;
    std::string_view sameSite;
    std::int64_t maxAge{-1};
    bool httpOnly{false};
    bool secure{false};
};

}  // namespace ruvia
