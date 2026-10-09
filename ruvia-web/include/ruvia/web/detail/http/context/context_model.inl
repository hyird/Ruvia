#pragma once

// Model-backed inline definitions for the public context API.

#include "ruvia/web/model_json.h"
#include "ruvia/web/model_types.h"

namespace ruvia {

template <typename t_type>
    requires detail::is_model<t_type>
inline http_response context::json(const t_type& value) const {
    auto body = to_json(value, {.resource_ = arena()});
    return json_serialized(body);
}

}  // namespace ruvia
