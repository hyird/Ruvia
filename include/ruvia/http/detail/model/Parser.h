#pragma once

#include <string_view>

#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/detail/model/JsonParser.h"
#include "ruvia/http/detail/model/FormParser.h"

// Internal aggregate parser header. Users should include ruvia/http/Model.h.

namespace ruvia::detail {

[[nodiscard]] inline bool contentTypeMatches(std::string_view contentType, std::string_view expected) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto semicolon = contentType.find(';');
    const auto mediaType = httpTrimOws(
        semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(mediaType, expected);
}

}  // namespace ruvia::detail
