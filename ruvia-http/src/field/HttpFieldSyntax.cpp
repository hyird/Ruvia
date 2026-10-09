#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"

// Shared field syntax without field-specific grammar. Values are normalized:
// field-line OWS belongs to wire parsers, not to the public field value.

namespace ruvia {

bool isValidHttpHeaderName(std::string_view name) noexcept {
    return detail::is_valid_http_field_name(name);
}

bool isValidHttpHeaderValue(std::string_view value) noexcept {
    return detail::is_valid_http_field_value(value);
}

bool isValidHttpStatusText(std::string_view value) noexcept {
    return isValidHttpHeaderValue(value);
}

}  // namespace ruvia
