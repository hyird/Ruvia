#pragma once

#include <string_view>

#include "ruvia/http/HttpFieldNameList.h"
#include "ruvia/http/HttpHeader.h"

#include "parser/HttpRequestTarget.h"

namespace ruvia::detail {

[[nodiscard]] inline bool isValidHttpCorsRequestMethod(std::string_view value) noexcept {
    return isValidHttpMethodToken(value);
}

[[nodiscard]] inline bool isValidHttpCorsRequestHeaderNames(std::string_view value) noexcept {
    http_field_name_list names(value);
    bool saw_name = false;
    while (names.next()) {
        saw_name = true;
    }
    // Access-Control-Request-Headers requires at least one field name, unlike
    // the general #field-name grammar.
    return saw_name && names.valid();
}

}  // namespace ruvia::detail
