#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_limits.h"

namespace ruvia::detail {

// Consumes one array directly from input and lets the visitor consume each
// element from the same cursor, positioned at the element's first byte. The
// visitor returns false to reject the array. input advances past ']' only when
// the whole array is accepted; nested values visited by the caller belong at
// depth + 1.
template <typename visitor_type>
[[nodiscard]] bool consume_json_array_elements(std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '[')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == ']') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        skip_json_whitespace(remaining);
        if (!static_cast<bool>(visitor_ref(remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

}  // namespace ruvia::detail
