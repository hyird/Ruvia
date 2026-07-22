#include "ruvia/http/HttpHeader.h"

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

#include <algorithm>

// Whether a field name, field value or reason phrase is legal on the wire
// (RFC 9110 section 5.1 and 5.5): the byte repertoire alone, with no field-
// specific grammar.

namespace ruvia {

bool isValidHttpHeaderName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    return std::ranges::all_of(name, [](char value) noexcept {
        return detail::isHttpTokenChar(static_cast<unsigned char>(value));
    });
}

bool isValidHttpHeaderValue(std::string_view value) noexcept {
    if (!value.empty()) {
        const auto first = static_cast<unsigned char>(value.front());
        const auto last = static_cast<unsigned char>(value.back());
        if (first == ' ' || first == '\t' || last == ' ' || last == '\t') {
            return false;
        }
    }
    return std::ranges::all_of(value, [](char c) noexcept {
        return detail::isHttpFieldValueChar(static_cast<unsigned char>(c));
    });
}

bool isValidHttpStatusText(std::string_view value) noexcept {
    return isValidHttpHeaderValue(value);
}

}  // namespace ruvia
