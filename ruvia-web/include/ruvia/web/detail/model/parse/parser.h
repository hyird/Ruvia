#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_media_type.h"
#include "ruvia/web/detail/model/parse/form_parser.h"
#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/model_types.h"

// Internal aggregate parser header. Users should include ruvia/web/model.h.

namespace ruvia::detail {

[[nodiscard]] inline bool content_type_matches(
    std::string_view content_type_value, std::string_view expected) noexcept {
    if (content_type_value.empty()) {
        return false;
    }
    return http_ascii_equals_ignore_case(::ruvia::http_media_type_only(content_type_value), expected);
}

}  // namespace ruvia::detail
