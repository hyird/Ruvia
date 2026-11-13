#pragma once

#include <concepts>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/json/json_scan.h"

// The single owner of the JSON array and object grammars, structural value
// skipping, and object field traversal. Skipping, typed model parsing and
// dynamic token traversal all walk containers through these functions.

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
    std::string* decoded_key, visitor_type&& visitor, key_budget_type&& consume_key_bytes) {
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
    std::string_view& input, std::size_t depth, std::string* decoded_key, visitor_type&& visitor) {
    return consume_json_object_members(
        input, depth, decoded_key, std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept;

template <typename element_budget_type>
[[nodiscard]] bool skip_json_array(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_array_elements(input, depth, [depth, &consume_element](std::string_view& element) noexcept {
        return consume_element() && skip_json_value(element, depth + 1, consume_element);
    });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_object(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
    return consume_json_object_members(input, depth, nullptr,
        [depth, &consume_element](const json_string_token&, std::string_view& value) noexcept {
            return skip_json_value(value, depth + 1, consume_element);
        });
}

template <typename element_budget_type>
[[nodiscard]] bool skip_json_value(std::string_view& input, std::size_t depth,
    element_budget_type& consume_element) noexcept {
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
            return skip_json_object(input, depth, consume_element);
        case '[':
            return skip_json_array(input, depth, consume_element);
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

[[nodiscard]] inline bool skip_json_value(std::string_view& input, std::size_t depth) noexcept {
    auto consume_element = []() noexcept { return true; };
    return skip_json_value(input, depth, consume_element);
}

enum class json_object_visit_result : unsigned char { complete,
    stopped,
    invalid };

// Consumes one object directly from input and lets the visitor consume each
// value from the same cursor, under the member's decoded key. RUVIA_MODEL uses
// this On-Demand-style traversal so a known value is parsed into its typed
// field without first scanning it to discover a raw slice and then parsing
// that slice again. consume_key_allocation is charged with the raw size of
// every escaped key before the visitor sees it.
template <typename visitor_type, typename key_allocation_type>
[[nodiscard]] bool consume_json_object_fields(std::string_view& input, std::size_t depth,
    visitor_type&& visitor, key_allocation_type&& consume_key_allocation) {
    auto& visitor_ref = visitor;
    std::string decoded_key;
    bool key_started = false;
    return consume_json_object_members(
        input, depth, &decoded_key,
        [&](const json_string_token& key, std::string_view& value_input) {
            key_started = false;
            if (key.encoding() == json_string_encoding::escaped) {
                return static_cast<bool>(visitor_ref(std::string_view(decoded_key), value_input));
            }
            return static_cast<bool>(visitor_ref(key.raw(), value_input));
        },
        [&](std::size_t bytes_value) noexcept {
            const bool first = !std::exchange(key_started, true);
            return consume_key_allocation(bytes_value, first);
        });
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(
    std::string_view& input, std::size_t depth, visitor_type&& visitor) {
    return consume_json_object_fields(input, depth,
        std::forward<visitor_type>(visitor), [](std::size_t, bool) noexcept { return true; });
}

// Visits every field of one complete JSON object body as a raw value slice
// (which may carry leading whitespace). Values are skipped at depth 1, so the
// whole body honours max_json_depth. The visitor returns false to stop.
template <typename visitor_type>
    requires std::predicate<visitor_type&, std::string_view, std::string_view>
[[nodiscard]] json_object_visit_result visit_json_object_fields(
    std::string_view body, visitor_type&& visitor) {
    auto input = body;
    auto& visitor_ref = visitor;
    bool stopped = false;
    const bool consumed = consume_json_object_fields(input, 0,
        [&visitor_ref, &stopped](std::string_view key, std::string_view& value_input) {
            const auto value_start = value_input;
            if (!skip_json_value(value_input, 1)) {
                return false;
            }
            const auto value = value_start.substr(0, value_start.size() - value_input.size());
            stopped = !visitor_ref(key, value);
            return !stopped;
        });
    if (stopped) {
        return json_object_visit_result::stopped;
    }
    if (!consumed) {
        return json_object_visit_result::invalid;
    }
    skip_json_whitespace(input);
    return input.empty() ? json_object_visit_result::complete : json_object_visit_result::invalid;
}

}  // namespace ruvia::detail
