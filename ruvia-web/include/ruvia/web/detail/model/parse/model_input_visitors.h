#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/model/model_input.h"
#include "ruvia/web/detail/model/parse/request_field_visitors.h"

namespace ruvia::detail {

template <typename visitor_type>
[[nodiscard]] bool visit_model_input_json_fields(const model_input& input, visitor_type&& visitor) {
    return visit_json_object_fields(
               resolved_pmr_resource_tag{}, input.view(), input.resource(), std::forward<visitor_type>(visitor)) != json_object_visit_result::invalid;
}

template <typename visitor_type>
[[nodiscard]] bool visit_model_input_form_fields(const model_input& input, visitor_type&& visitor) {
    if (input.kind() == model_input_kind::form_fields) {
        const auto* const fields_value = input.fields();
        return fields_value == nullptr ? true
                                       : visit_decoded_form_fields(*fields_value, std::forward<visitor_type>(visitor));
    }
    return visit_form_object_fields(
        resolved_pmr_resource_tag{}, input.view(), input.resource(), std::forward<visitor_type>(visitor));
}

}  // namespace ruvia::detail
