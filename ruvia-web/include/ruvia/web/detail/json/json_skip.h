#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/json/json_array_elements.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_limits.h"
#include "ruvia/web/detail/json/json_number.h"
#include "ruvia/web/detail/json/json_string.h"

namespace ruvia::detail {

[[nodiscard]] inline bool skip_json_value(std::string_view& input) noexcept;
[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept;

[[nodiscard]] inline bool skip_json_array(std::string_view& input, std::size_t depth) noexcept {
    return consume_json_array_elements(input, depth, [depth](std::string_view& element) noexcept {
        return skip_json_value(element, depth + 1);
    });
}

[[nodiscard]] inline bool skip_json_object(std::string_view& input, std::size_t depth) noexcept {
    if (depth > max_json_depth) {
        return false;
    }
    if (!consume_json_char(input, '{')) {
        return false;
    }
    skip_json_whitespace(input);
    if (!input.empty() && input.front() == '}') {
        input.remove_prefix(1);
        return true;
    }

    while (parse_json_string(input).has_value()) {
        if (!consume_json_char(input, ':') || !skip_json_value(input, depth + 1)) {
            return false;
        }
        skip_json_whitespace(input);
        if (!input.empty() && input.front() == '}') {
            input.remove_prefix(1);
            return true;
        }
        if (!consume_json_char(input, ',')) {
            return false;
        }
    }

    return false;
}

[[nodiscard]] inline bool skip_json_object(std::string_view& input) noexcept {
    return skip_json_object(input, 0);
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept {
    if (depth > max_json_depth) {
        return false;
    }
    skip_json_whitespace(input);
    if (input.empty()) {
        return false;
    }

    switch (input.front()) {
        case '"':
            return parse_json_string(input).has_value();
        case '{':
            // skip_json_object/skip_json_array already add depth+1 for their child values; passing
            // depth+1 here too would double-count and enforce half the documented max_json_depth.
            return skip_json_object(input, depth);
        case '[':
            return skip_json_array(input, depth);
        case 't':
            return consume_json_literal(input, "true");
        case 'f':
            return consume_json_literal(input, "false");
        case 'n':
            return consume_json_literal(input, "null");
        default:
            return skip_json_number_token(input);
    }
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input) noexcept {
    return skip_json_value(input, 0);
}

}  // namespace ruvia::detail
