#pragma once

#include <cstdint>

#include "ruvia/http/http_client.h"

namespace ruvia::detail {

[[nodiscard]] constexpr std::uint16_t http_scheme_default_port(http_scheme scheme) noexcept {
    return scheme == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80};
}

[[nodiscard]] inline bool http_origin_uses_default_port(const http_origin_view& origin) noexcept {
    return origin.port() == http_scheme_default_port(origin.scheme());
}

}  // namespace ruvia::detail
