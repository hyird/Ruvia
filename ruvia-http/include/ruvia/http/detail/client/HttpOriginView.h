#pragma once

#include <cstdint>

#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

[[nodiscard]] constexpr std::uint16_t httpSchemeDefaultPort(HttpScheme scheme) noexcept {
    return scheme == HttpScheme::kHttps ? std::uint16_t{443} : std::uint16_t{80};
}

[[nodiscard]] inline bool httpOriginUsesDefaultPort(const HttpOriginView& origin) noexcept {
    return origin.port() == httpSchemeDefaultPort(origin.scheme());
}

}  // namespace ruvia::detail
