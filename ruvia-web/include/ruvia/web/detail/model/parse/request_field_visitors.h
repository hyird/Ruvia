#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/model/model_input.h"
#include "ruvia/web/detail/model/parse/form_parser.h"

namespace ruvia::detail {

template <typename visitor_type>
[[nodiscard]] bool visit_form_object_fields(resolved_pmr_resource_tag, std::string_view body,
    std::pmr::memory_resource* resource, visitor_type&& visitor) {
    bool valid = true;
    auto& visitor_ref = visitor;
    std::pmr::string decoded_name(resource);
    // Form decoding keeps malformed '%' sequences literally (WHATWG), so the
    // body needs no separate percent-encoding pass.
    const bool completed = ruvia::visit_url_encoded_pairs(body, [&](std::string_view name,
                                                                    std::string_view value) {
        const auto decoded = ruvia::decode_url_component(name, decoded_name, url_decode_mode::form);
        if (!decoded.has_value()) {
            valid = false;
            return false;
        }
        return dispatch_json_object_field_visitor(visitor_ref, *decoded, value);
    });
    return completed && valid;
}

template <typename fields_type, typename visitor_type>
[[nodiscard]] bool visit_decoded_form_fields(const fields_type& fields_value, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    for (const auto& field : fields_value) {
        if (!dispatch_json_object_field_visitor(visitor_ref, field.name(), field.value())) {
            break;
        }
    }
    return true;
}

template <typename visitor_type>
[[nodiscard]] bool visit_model_input_form_fields(const model_input& input, visitor_type&& visitor) {
    if (input.kind() == model_input_kind::form_fields) {
        const auto* const fields_value = input.fields();
        return fields_value == nullptr
                   ? true
                   : visit_decoded_form_fields(*fields_value, std::forward<visitor_type>(visitor));
    }
    return visit_form_object_fields(
        resolved_pmr_resource_tag{}, input.view(), input.resource(), std::forward<visitor_type>(visitor));
}

}  // namespace ruvia::detail
