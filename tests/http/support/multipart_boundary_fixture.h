#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/multipart_parser.h"

namespace ruvia::testing {

// A multipart/form-data Content-Type whose boundary parameter is `value` as a
// quoted-string, so every byte reaches the boundary rule unchanged.
[[nodiscard]] inline std::string multipart_boundary_content_type(std::string_view value) {
    std::string content_type("multipart/form-data; boundary=\"");
    for (const char byte : value) {
        if (byte == '"' || byte == '\\') {
            content_type.push_back('\\');
        }
        content_type.push_back(byte);
    }
    content_type.push_back('"');
    return content_type;
}

// Builds a boundary through parse_multipart_boundary, the Content-Type route.
[[nodiscard]] inline ruvia::multipart_boundary make_multipart_boundary(std::string_view value) {
    const auto parsed = ruvia::parse_multipart_boundary(multipart_boundary_content_type(value));
    if (const auto* boundary = parsed.boundary()) {
        return *boundary;
    }
    throw std::invalid_argument("invalid test multipart boundary");
}

}  // namespace ruvia::testing
