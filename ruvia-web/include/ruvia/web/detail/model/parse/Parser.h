#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpMediaType.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/parse/FormParser.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"

// Internal aggregate parser header. Users should include ruvia/web/Model.h.

namespace ruvia::detail {

[[nodiscard]] inline bool contentTypeMatches(
    std::string_view contentType, std::string_view expected) noexcept {
    if (contentType.empty()) {
        return false;
    }
    return httpAsciiEqualsIgnoreCase(::ruvia::httpMediaTypeOnly(contentType), expected);
}

}  // namespace ruvia::detail
