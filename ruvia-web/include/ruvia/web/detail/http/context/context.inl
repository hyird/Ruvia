#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/model_types.h"

namespace ruvia {

template <typename t_type>
inline request_state_binding_type<t_type> context::bind_request_state(const t_type& value) {
    return request_bindings().bind_state(value);
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>& context::request_state() const {
    return request_bindings().get_state<t_type>();
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>* context::try_request_state() const noexcept {
    return request_bindings().try_get_state<t_type>();
}

template <typename t_type>
    requires detail::is_model<t_type>
inline http_response context::json(const t_type& value) const {
    auto body = to_json(value, {.resource_ = arena()});
    return json_serialized(body);
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
inline request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model) {
    return context_value.request_bindings().bind_validated(model);
}

}  // namespace ruvia::detail
