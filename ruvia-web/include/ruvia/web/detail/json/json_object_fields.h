#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_limits.h"
#include "ruvia/web/detail/json/json_skip.h"
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

// Consumes one object directly from input and lets the visitor consume each
// value from the same cursor. RUVIA_MODEL uses this On-Demand-style traversal
// so a known value is parsed into its typed field without first scanning it to
// discover a raw slice and then parsing that slice again.
template <typename visitor_type, typename key_allocation_type>
[[nodiscard]] bool consume_json_object_fields(resolved_pmr_resource_tag, std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, visitor_type&& visitor,
    key_allocation_type&& consume_key_allocation) {
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
            if (!consume_key_allocation(key->raw().size())) {
                return false;
            }
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

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(resolved_pmr_resource_tag, std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, visitor_type&& visitor) {
    return consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource, depth,
        std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}

// Visits every field of one complete JSON object body as a raw value slice
// (which may carry leading whitespace). Values are skipped at depth 1, so the
// whole body honours max_json_depth.
template <typename visitor_type>
[[nodiscard]] json_object_visit_result visit_json_object_fields(resolved_pmr_resource_tag, std::string_view body,
    std::pmr::memory_resource* resource, visitor_type&& visitor) {
    auto input = body;
    auto& visitor_ref = visitor;
    bool stopped = false;
    const bool consumed = consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource, 0,
        [&visitor_ref, &stopped](std::string_view key, std::string_view& value_input) {
            const auto value_start = value_input;
            if (!skip_json_value(value_input, 1)) {
                return false;
            }
            const auto value = value_start.substr(0, value_start.size() - value_input.size());
            stopped = !dispatch_json_object_field_visitor(visitor_ref, key, value);
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
