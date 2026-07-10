#pragma once

#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpTypes.h"

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace ruvia::detail {

struct RequestTargetView {
    std::string_view path;
    std::string_view query;
    std::string_view authority;
    std::uint16_t defaultPort{0};
};

[[nodiscard]] inline bool isValidRequestTargetBytes(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < target.size(); ++i) {
        const auto c = static_cast<unsigned char>(target[i]);
        if (c <= 0x20 || c == 0x7F || c == '#' || c == '\\') {
            return false;
        }
        if (c == '%') {
            if (i + 2 >= target.size() ||
                decodeHexNibble(target[i + 1]) < 0 ||
                decodeHexNibble(target[i + 2]) < 0) {
                return false;
            }
            i += 2;
        }
    }
    return true;
}

[[nodiscard]] inline bool isValidOriginFormTarget(std::string_view target) noexcept {
    if (target == "*") {
        return true;
    }
    return !target.empty() && target.front() == '/' && isValidRequestTargetBytes(target);
}

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
