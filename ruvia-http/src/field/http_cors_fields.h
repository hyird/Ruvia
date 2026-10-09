#pragma once

#include <string_view>

#include "ruvia/http/http_field_name_list.h"
#include "ruvia/http/http_header.h"

#include "parser/http_request_target.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_valid_http_cors_request_method(std::string_view value) noexcept {
    return is_valid_http_method_token(value);
}

[[nodiscard]] inline bool is_valid_http_cors_request_header_names(std::string_view value) noexcept {
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
