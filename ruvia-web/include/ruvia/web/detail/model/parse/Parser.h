#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/http/detail/util/HttpOws.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"
#include "ruvia/web/detail/model/parse/FormParser.h"

// Internal aggregate parser header. Users should include ruvia/web/Model.h.

namespace ruvia::detail {

[[nodiscard]] inline bool contentTypeMatches(
    std::string_view contentType, std::string_view expected) noexcept {
    if (contentType.empty()) {
        return false;
    }
    const auto semicolon = contentType.find(';');
    const auto mediaType = httpTrimOws(
        semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    return httpAsciiEqualsIgnoreCase(mediaType, expected);
}

}  // namespace ruvia::detail
