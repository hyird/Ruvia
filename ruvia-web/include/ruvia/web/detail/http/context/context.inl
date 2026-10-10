#pragma once

#include <cstddef>
#include <string_view>

#include "ruvia/web/detail/http/context/request_bindings.h"

namespace ruvia::detail {

// A character array may be a partly filled buffer: its bytes end at the first
// NUL, or at the array bound when it holds none.
template <std::size_t n>
[[nodiscard]] constexpr std::string_view char_array_bytes(const char (&value)[n]) noexcept {
    const std::string_view bytes(value, n);
    return bytes.substr(0, bytes.find('\0'));
}

}  // namespace ruvia::detail

namespace ruvia {

template <std::size_t n>
inline http_response context::body(const char (&value)[n]) const {
    return body(detail::char_array_bytes(value));
}

template <std::size_t n>
inline http_response context::text(const char (&body)[n]) const {
    return text(detail::char_array_bytes(body));
}

template <std::size_t n>
inline http_response context::html(const char (&body)[n]) const {
    return html(detail::char_array_bytes(body));
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
