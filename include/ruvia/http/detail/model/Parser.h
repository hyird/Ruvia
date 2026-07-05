#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/detail/AsciiCase.h"
#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/JsonParser.h"
#include "ruvia/http/detail/model/FormParser.h"

// Internal aggregate parser header. Users should include ruvia/http/Model.h.

namespace ruvia::detail {

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
    return asciiEqualsIgnoreCase(mediaType, expected);
}

}  // namespace ruvia::detail
