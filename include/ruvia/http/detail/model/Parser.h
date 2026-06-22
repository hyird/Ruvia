#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/JsonParser.h"
#include "ruvia/http/detail/model/FormParser.h"

// Internal aggregate parser header. Users should include ruvia/http/Model.h.

namespace ruvia::detail {

[[nodiscard]] inline unsigned char modelLowerAscii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] inline bool modelAsciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t i = 0; i < left.size(); ++i) {
        if (modelLowerAscii(static_cast<unsigned char>(left[i])) !=
            modelLowerAscii(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }

    return true;
}

[[nodiscard]] inline std::string_view modelTrimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline bool contentTypeMatches(std::string_view contentType, std::string_view expected) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto semicolon = contentType.find(';');
    const auto mediaType = modelTrimOws(
        semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    return modelAsciiEqualsIgnoreCase(mediaType, expected);
}

}  // namespace ruvia::detail
