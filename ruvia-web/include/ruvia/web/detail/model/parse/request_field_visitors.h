#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/json/json_object_fields.h"
#include "ruvia/web/detail/model/parse/form_parser.h"

namespace ruvia::detail {

template <typename visitor_type>
[[nodiscard]] bool visit_raw_form_fields(std::string_view body, visitor_type&& visitor) {
    if (!ruvia::validate_url_encoding(body)) {
        return false;
    }
    return ruvia::visit_url_encoded_pairs(body, std::forward<visitor_type>(visitor));
}

template <typename visitor_type>
[[nodiscard]] bool visit_form_object_fields(resolved_pmr_resource_tag, std::string_view body,
    std::pmr::memory_resource* resource, visitor_type&& visitor) {
    bool valid = true;
    auto& visitor_ref = visitor;
    const bool completed = visit_raw_form_fields(body, [&](std::string_view name,
                                                           std::string_view value) {
        if (!ruvia::has_url_encoding(name, url_decode_mode::form)) {
            return dispatch_json_object_field_visitor(visitor_ref, name, value);
        }

        auto decoded_name =
            ruvia::decode_url_component(name, {.mode_ = url_decode_mode::form, .resource_ = resource});
        if (!decoded_name.has_value()) {
            valid = false;
            return false;
        }
        return dispatch_json_object_field_visitor(visitor_ref, std::string_view(*decoded_name), value);
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

}  // namespace ruvia::detail
