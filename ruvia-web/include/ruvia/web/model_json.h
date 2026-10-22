#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/parse/json_writer.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/model_types.h"
#include "ruvia/web/validation.h"

namespace ruvia {

// Owns parsed data in options.resource. Supports models, Ruvia scalar values,
// strings, bytes and arrays of these types. Model types also evaluate their
// declared field validation rules.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] t_type from_json(std::string_view body, model_parse_options options = {}) {
    auto* const resource = pmr_resource_or_default(options.resource_);
    auto parsed_value = detail::parse_json_document<t_type>(
        body, resource, detail::model_string_storage::owned, options);
    if (!parsed_value) {
        throw std::invalid_argument("invalid JSON value");
    }
    if constexpr (detail::is_model<t_type>) {
        validator validator_value({.resource_ = resource});
        validate_model(*parsed_value, validator_value);
        validator_value.throw_if_invalid();
    }
    return std::move(*parsed_value);
}
// Serializes current values without invoking field rules or initializers.
// The output is always well-formed UTF-8 (RFC 8259 §8.1): a string value holding
// ill-formed UTF-8 has each maximal ill-formed subpart replaced by U+FFFD.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] inline std::pmr::string to_json(const t_type& value, model_serialize_options options = {}) {
    std::pmr::string output(pmr_resource_or_default(options.resource_));
    // MSVC reserve(n) can leave no room for the trailing NUL, so resize(n)
    // allocates again. One extra byte stays in the same allocation.
    output.reserve(detail::json_size_hint_value(value) + 1);
    detail::append_json_value(output, value);
    return output;
}

}  // namespace ruvia
