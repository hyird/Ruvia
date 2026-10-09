#pragma once

#include "ruvia/web/detail/http/context/request_bindings.h"

namespace ruvia {

template <std::size_t n>
inline http_response context::body(const char (&value)[n]) const {
    const auto size = n > 0 && value[n - 1] == '\0' ? n - 1 : n;
    return body_static_view(std::string_view(value, size));
}

template <std::size_t n>
inline http_response context::text(const char (&body)[n]) const {
    const auto size = n > 0 && body[n - 1] == '\0' ? n - 1 : n;
    return text_static_view(std::string_view(body, size));
}

template <std::size_t n>
inline http_response context::html(const char (&body)[n]) const {
    const auto size = n > 0 && body[n - 1] == '\0' ? n - 1 : n;
    return html_static_view(std::string_view(body, size));
}

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

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
inline request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model) {
    return context_value.request_bindings().bind_validated(model);
}

template <typename t_type>
inline request_binding_handle<t_type> bind_validated_json_model(
    context& context_value, const t_type& model, std::string_view raw_json) {
    return context_value.request_bindings().bind_validated(model, raw_json);
}

}  // namespace ruvia::detail
