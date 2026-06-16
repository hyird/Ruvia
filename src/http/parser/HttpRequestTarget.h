#pragma once

#include "ruvia/http/HttpTypes.h"

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

struct RequestTargetView {
    std::string_view path;
    std::string_view query;
    std::string_view authority;
    std::uint16_t defaultPort{0};
};

[[nodiscard]] bool isValidHostHeader(std::string_view value) noexcept;
[[nodiscard]] bool parseRequestTarget(
    HttpMethod method,
    std::string_view target,
    RequestTargetView& output) noexcept;
[[nodiscard]] bool authorityMatchesHost(
    std::string_view authority,
    std::string_view host,
    std::uint16_t defaultPort) noexcept;

}  // namespace ruvia::detail
