#pragma once

#include <string_view>

#include "ruvia/http/detail/parser/http_parser_syntax.h"

namespace ruvia::detail {

inline constexpr auto binary_field_name_char_table = [] {
    auto table_value = http_token_char_table;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table_value[byte] = false;
    }
    return table_value;
}();

// HTTP/2 regular field names are nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (name.size() - index >= 4) {
        const auto* bytes_value = reinterpret_cast<const unsigned char*>(name.data() + index);
        const bool first = binary_field_name_char_table[bytes_value[0]];
        const bool second = binary_field_name_char_table[bytes_value[1]];
        const bool third = binary_field_name_char_table[bytes_value[2]];
        const bool fourth = binary_field_name_char_table[bytes_value[3]];
        if (!(first & second & third & fourth)) {
            return false;
        }
        index += 4;
    }
    while (index < name.size()) {
        if (!binary_field_name_char_table[static_cast<unsigned char>(name[index])]) {
            return false;
        }
        ++index;
    }
    return true;
}

}  // namespace ruvia::detail
