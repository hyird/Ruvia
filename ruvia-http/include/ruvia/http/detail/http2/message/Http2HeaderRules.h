#pragma once

#include <string_view>

#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2HeaderNameHasUppercase(std::string_view name) noexcept {
    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool http2FieldValueHasLeadingOrTrailingWhitespace(
    std::string_view value) noexcept {
    const auto asciiWhitespace = [](char ch) noexcept { return ch == ' ' || ch == '\t'; };
    return !value.empty() && (asciiWhitespace(value.front()) || asciiWhitespace(value.back()));
}

[[nodiscard]] inline bool http2IsValidRegularHeader(
    std::string_view name, std::string_view value) noexcept {
    if (name.empty() || name.front() == ':') {
        return false;
    }
    if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value) ||
        http2FieldValueHasLeadingOrTrailingWhitespace(value)) {
        return false;
    }
    if (http2HeaderNameHasUppercase(name)) {
        return false;
    }
    if (is_forbidden_http_binary_connection_field(name)) {
        return false;
    }
    // `trailers` is an unprefixed ABNF literal in RFC 9110 Section 10.1.4,
    // hence it is case-insensitive (RFC 7405 Section 2.1). HTTP/2 restricts TE
    // to that sole value, but does not make the field-value keyword lowercase.
    return name != "te" || httpAsciiEqualsIgnoreCase(value, "trailers");
}

// Decoded HTTP/2 field names are already required to be lowercase. The TE
// exception above belongs only to requests (RFC 9113 Section 8.2.2); a response
// carrying TE is malformed even when its value is exactly "trailers".
[[nodiscard]] inline bool http2IsValidDecodedResponseHeader(
    std::string_view name, std::string_view value) noexcept {
    return http2IsValidRegularHeader(name, value) && !is_forbidden_http_binary_response_field(name);
}

}  // namespace ruvia::detail
