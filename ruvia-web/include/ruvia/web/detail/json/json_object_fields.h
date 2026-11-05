#pragma once

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/json/json_containers.h"
#include "ruvia/web/detail/json/json_lex.h"
#include "ruvia/web/detail/json/json_skip.h"
#include "ruvia/web/detail/json/json_string.h"

namespace ruvia::detail {

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
[[nodiscard]] bool consume_json_object_fields(resolved_pmr_resource_tag, std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, visitor_type&& visitor,
    key_allocation_type&& consume_key_allocation) {
    auto& visitor_ref = visitor;
    std::pmr::string decoded_key(pmr_resource_or_default(resource));
    return consume_json_object_members(input, depth, &decoded_key,
        [&](const json_string_token& key, std::string_view& value_input) {
            if (key.encoding() == json_string_encoding::escaped) {
                if (!consume_key_allocation(key.raw().size())) {
                    return false;
                }
                return static_cast<bool>(visitor_ref(std::string_view(decoded_key), value_input));
            }
            return static_cast<bool>(visitor_ref(key.raw(), value_input));
        });
}

template <typename visitor_type>
[[nodiscard]] bool consume_json_object_fields(resolved_pmr_resource_tag, std::string_view& input,
    std::pmr::memory_resource* resource, std::size_t depth, visitor_type&& visitor) {
    return consume_json_object_fields(resolved_pmr_resource_tag{}, input, resource, depth,
        std::forward<visitor_type>(visitor), [](std::size_t) noexcept { return true; });
}

// Visits every field of one complete JSON object body as a raw value slice
// (which may carry leading whitespace). Values are skipped at depth 1, so the
// whole body honours max_json_depth. The visitor returns false to stop.
template <typename visitor_type>
    requires std::predicate<visitor_type&, std::string_view, std::string_view>
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
