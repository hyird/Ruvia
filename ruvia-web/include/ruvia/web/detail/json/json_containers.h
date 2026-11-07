#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_limits.h"
#include "ruvia/web/detail/json/json_string.h"

// The single owner of the JSON array and object grammars. Skipping, typed
// model parsing and dynamic token traversal all walk containers through these
// two functions.

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

// Consumes one object directly from input. For each member the visitor
// receives the scanned key token and consumes the value from the same cursor
// (positioned after ':'); it returns false to reject the object. When
// `decoded_key` is non-null, escaped keys are decoded into it by the key scan
// itself (see parse_json_string). input advances past '}' only when the whole
// object is accepted; nested values visited by the caller belong at depth + 1.
template <typename visitor_type, typename key_budget_type>
[[nodiscard]] bool consume_json_object_members(std::string_view& input, std::size_t depth,
    std::pmr::string* decoded_key, visitor_type&& visitor, key_budget_type&& consume_key_bytes) {
    if (depth > max_json_depth) {
        return false;
    }

    auto remaining = input;
    if (!consume_json_char(remaining, '{')) {
        return false;
    }
    skip_json_whitespace(remaining);
    if (!remaining.empty() && remaining.front() == '}') {
        remaining.remove_prefix(1);
        input = remaining;
        return true;
    }

    auto& visitor_ref = visitor;
    while (true) {
        const auto key = parse_json_string(remaining, decoded_key, consume_key_bytes);
        if (!key.has_value() || !consume_json_char(remaining, ':')) {
            return false;
        }
        if (!static_cast<bool>(visitor_ref(*key, remaining))) {
            return false;
        }

        skip_json_whitespace(remaining);
        if (!remaining.empty() && remaining.front() == '}') {
            remaining.remove_prefix(1);
            input = remaining;
            return true;
        }
        if (!consume_json_char(remaining, ',')) {
            return false;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_members(
    std::string_view& input, std::size_t depth, std::pmr::string* decoded_key, visitor_type&& visitor) {
    return consume_json_object_members(
        input, depth, decoded_key, std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}
}  // namespace ruvia::detail
