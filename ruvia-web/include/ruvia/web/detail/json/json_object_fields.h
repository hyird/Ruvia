#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/json/json_scanner.h"
#include "ruvia/web/detail/json/json_string.h"

namespace ruvia::detail {

template <typename visitor_type>
[[nodiscard]] bool dispatch_json_object_field_visitor(
    visitor_type& visitor, std::string_view key, std::string_view value) {
    if constexpr (requires {
                      { visitor(key, value) } -> std::convertible_to<bool>;
                  }) {
        return static_cast<bool>(visitor(key, value));
    } else {
        visitor(key, value);
        return true;
    }
}

enum class json_object_visit_result : unsigned char { complete,
    stopped,
    invalid };

template <typename visitor_type>
[[nodiscard]] json_object_visit_result visit_json_object_fields(resolved_pmr_resource_tag, std::string_view body,
    std::pmr::memory_resource* resource, visitor_type&& visitor) {
    auto input = body;
    if (!consume_json_char(input, '{')) {
        return json_object_visit_result::invalid;
    }
    skip_json_whitespace(input);
    if (!input.empty() && input.front() == '}') {
        input.remove_prefix(1);
        skip_json_whitespace(input);
        return input.empty() ? json_object_visit_result::complete : json_object_visit_result::invalid;
    }

    auto& visitor_ref = visitor;
    while (true) {
        const auto key = parse_json_string(input);
        if (!key.has_value() || !consume_json_char(input, ':')) {
            return json_object_visit_result::invalid;
        }

        const auto value_start = input;
        if (!skip_json_value(input)) {
            return json_object_visit_result::invalid;
        }
        const auto consumed = value_start.size() - input.size();
        const auto value = value_start.substr(0, consumed);
        if (key->encoding() == json_string_encoding::escaped) {
            auto decoded_key = decode_json_string(key->raw(), resource);
            if (!decoded_key.has_value()) {
                return json_object_visit_result::invalid;
            }
            if (!dispatch_json_object_field_visitor(visitor_ref, std::string_view(*decoded_key), value)) {
                return json_object_visit_result::stopped;
            }
        } else if (!dispatch_json_object_field_visitor(visitor_ref, key->raw(), value)) {
            return json_object_visit_result::stopped;
        }

        skip_json_whitespace(input);
        if (!input.empty() && input.front() == '}') {
            input.remove_prefix(1);
            skip_json_whitespace(input);
            return input.empty() ? json_object_visit_result::complete : json_object_visit_result::invalid;
        }
        if (!consume_json_char(input, ',')) {
            return json_object_visit_result::invalid;
        }
    }
}

template <typename visitor_type>
[[nodiscard]] json_object_visit_result visit_json_object_fields(
    std::string_view body, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    return visit_json_object_fields(resolved_pmr_resource_tag{}, body, pmr_resource_or_default(resource),
        std::forward<visitor_type>(visitor));
}

// Consumes one object directly from input and lets the visitor consume each
// value from the same cursor. RUVIA_MODEL uses this On-Demand-style traversal
// so a known value is parsed into its typed field without first scanning it to
// discover a raw slice and then parsing that slice again.
template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(resolved_pmr_resource_tag, std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, visitor_type&& visitor) {
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
        const auto key = parse_json_string(remaining);
        if (!key.has_value() || !consume_json_char(remaining, ':')) {
            return false;
        }

        bool consumed = false;
        if (key->encoding() == json_string_encoding::escaped) {
            auto decoded_key = decode_json_string(key->raw(), resource);
            if (!decoded_key.has_value()) {
                return false;
            }
            consumed = static_cast<bool>(visitor_ref(std::string_view(*decoded_key), remaining));
        } else {
            consumed = static_cast<bool>(visitor_ref(key->raw(), remaining));
        }
        if (!consumed) {
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

}  // namespace ruvia::detail
