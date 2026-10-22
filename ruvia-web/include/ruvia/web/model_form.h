#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/validation.h"

namespace ruvia {

template <typename t_type>
    requires detail::is_model<t_type>
[[nodiscard]] t_type from_form(std::string_view body, model_parse_options options = {}) {
    auto* const resource = pmr_resource_or_default(options.resource_);
    auto parsed_value = detail::model_parse_access::parse_form_owned<t_type>(body, resource);
    if (!parsed_value) {
        throw std::invalid_argument("invalid form value");
    }
    validator validator_value({.resource_ = resource});
    validate_model(*parsed_value, validator_value);
    validator_value.throw_if_invalid();
    return std::move(*parsed_value);
}
}  // namespace ruvia
