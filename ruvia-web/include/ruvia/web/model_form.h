#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia {

template <typename t_type>
    requires detail::is_model<t_type>
[[nodiscard]] std::optional<t_type> from_form(std::string_view body, model_parse_options options = {}) {
    return detail::model_parse_access::parse_form_owned<t_type>(
        body, detail::pmr_resource_or_default(options.resource_));
}

}  // namespace ruvia
