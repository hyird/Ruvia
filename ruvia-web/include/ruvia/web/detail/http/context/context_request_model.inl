#pragma once

// Model-backed templates for the request-only public facade. These depend only
// on context_request's narrow bridge, so context_request.h is self-contained and
// does not require the complete response/state context definition.

#include <optional>
#include <utility>

#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/model_json.h"

namespace ruvia {

template <typename t_type>
task<std::optional<t_type>> context_request::json_if_model_task(const context* context_value) {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    if (!context_content_type_matches(context_value, "application/json")) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_text_task(context_value);
    auto parsed_value =
        detail::model_parse_access::parse_json_borrowed<t_type>(request_body, context_resource(context_value));
    if (!parsed_value) {
        // Once Content-Type selects JSON, malformed JSON is a 400 rather than
        // an absent optional format. This keeps json_if<T>() from turning an
        // explicitly JSON request into a successful fallback.
        detail::throw_invalid_json_body();
    }
    co_return std::move(*parsed_value);
}

template <typename t_type>
scoped_operation<std::optional<t_type>> context_request::json_if() const {
    return ::ruvia::make_scoped_operation(
        context_operation_scope(context_), json_if_model_task<t_type>(context_));
}

template <typename t_type>
task<std::optional<t_type>> context_request::form_if_model_task(const context* context_value) {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    if (!context_content_type_matches(context_value, "application/x-www-form-urlencoded")) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_text_task(context_value);
    auto parsed_value =
        detail::model_parse_access::parse_form_borrowed<t_type>(request_body, context_resource(context_value));
    if (!parsed_value) {
        // The selected form media type makes a malformed body a client error;
        // nullopt is reserved for a different Content-Type.
        detail::throw_invalid_form_body();
    }
    co_return std::move(*parsed_value);
}

template <typename t_type>
scoped_operation<std::optional<t_type>> context_request::form_if() const {
    return ::ruvia::make_scoped_operation(
        context_operation_scope(context_), form_if_model_task<t_type>(context_));
}

template <typename t_type>
inline const t_type& context_request::validated() const {
    return request_bindings().get_validated<t_type>();
}

template <typename t_type>
inline validated_json<t_type> context_request::validated_json() const {
    return request_bindings().get_validated_json<t_type>();
}

}  // namespace ruvia
