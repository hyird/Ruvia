#pragma once

#include <string_view>

#include "ruvia/http/HttpFieldWhitespace.h"
#include "ruvia/http/HttpOrigin.h"

namespace ruvia::detail {

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool is_valid_http_origin_field_value(std::string_view value) noexcept {
    value = httpTrimOws(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!is_valid_http_serialized_origin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

}  // namespace ruvia::detail
