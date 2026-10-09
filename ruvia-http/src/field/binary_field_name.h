#pragma once

#include <string_view>

#include "ruvia/http/detail/parser/HttpParserSyntax.h"

namespace ruvia::detail {

inline constexpr auto binary_field_name_char_table = [] {
    auto table = kHttpTokenCharTable;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table[byte] = false;
    }
    return table;
}();

// HTTP/2 and HTTP/3 regular field names are nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char byte : name) {
        if (!binary_field_name_char_table[byte]) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail
