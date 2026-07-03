#pragma once

#include <string_view>

#include "../../http/HeaderTokenUtils.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2HeaderNameHasUppercase(std::string_view name) noexcept {
    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool http2IsForbiddenConnectionHeader(std::string_view name) noexcept {
    return name == "connection" ||
        name == "keep-alive" ||
        name == "proxy-connection" ||
        name == "transfer-encoding" ||
        name == "upgrade";
}

[[nodiscard]] inline bool http2IsForbiddenUpgradedRequestHeader(std::string_view name) noexcept {
    return httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade") ||
        httpAsciiEqualsIgnoreCase(name, "http2-settings") ||
        httpAsciiEqualsIgnoreCase(name, "keep-alive") ||
        httpAsciiEqualsIgnoreCase(name, "proxy-connection") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding");
}

[[nodiscard]] inline bool http2IsValidRegularHeader(
    std::string_view name,
    std::string_view value) noexcept {
    if (name.empty() || name.front() == ':') {
        return false;
    }
    if (http2HeaderNameHasUppercase(name)) {
        return false;
    }
    if (http2IsForbiddenConnectionHeader(name)) {
        return false;
    }
    return name != "te" || value == "trailers";
}

}  // namespace ruvia::detail
