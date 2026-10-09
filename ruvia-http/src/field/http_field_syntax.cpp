#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"

// Shared field syntax without field-specific grammar. Values are normalized:
// field-line OWS belongs to wire parsers, not to the public field value.

namespace ruvia {

bool is_valid_http_header_name(std::string_view name) noexcept {
    return detail::is_valid_http_field_name(name);
}

bool is_valid_http_header_value(std::string_view value) noexcept {
    return detail::is_valid_http_field_value(value);
}

bool is_valid_http_status_text(std::string_view value) noexcept {
    return is_valid_http_header_value(value);
}

}  // namespace ruvia
