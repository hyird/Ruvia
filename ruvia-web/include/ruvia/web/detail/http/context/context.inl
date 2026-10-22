#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/model.h"
#include "ruvia/web/model_types.h"
#include "ruvia/web/validation.h"

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

template <typename t_type>
scoped_operation<std::optional<t_type>> context_request::json_if() const {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    const auto run = [](const context* context_value) -> task<std::optional<t_type>> {
        if (!detail::content_type_matches(
                context_value->request_.header("Content-Type").value_or(std::string_view{}),
                "application/json")) {
            co_return std::nullopt;
        }
        const auto request_body = co_await context_value->req().text();
        auto* const resource = context_value->arena();
        auto parsed_value =
            detail::model_parse_access::parse_json_borrowed_partial<t_type>(request_body, resource);
        if (!parsed_value) {
            // Once Content-Type selects JSON, malformed JSON is a 400 rather than
            // an absent optional format. This keeps json_if<T>() from turning an
            // explicitly JSON request into a successful fallback.
            detail::throw_invalid_json_body();
        }
        validator validator_value({.resource_ = resource});
        detail::model_validation_access::validate_model(*parsed_value, validator_value);
        validator_value.throw_if_invalid();
        co_return std::move(*parsed_value);
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
}

template <typename t_type>
scoped_operation<std::optional<t_type>> context_request::form_if() const {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    const auto run = [](const context* context_value) -> task<std::optional<t_type>> {
        if (!detail::content_type_matches(
                context_value->request_.header("Content-Type").value_or(std::string_view{}),
                "application/x-www-form-urlencoded")) {
            co_return std::nullopt;
        }
        const auto request_body = co_await context_value->req().text();
        auto* const resource = context_value->arena();
        auto parsed_value =
            detail::model_parse_access::parse_form_borrowed_partial<t_type>(request_body, resource);
        if (!parsed_value) {
            // The selected form media type makes a malformed body a client error;
            // nullopt is reserved for a different Content-Type.
            detail::throw_invalid_form_body();
        }
        validator validator_value({.resource_ = resource});
        detail::model_validation_access::validate_model(*parsed_value, validator_value);
        validator_value.throw_if_invalid();
        co_return std::move(*parsed_value);
    };
    return ::ruvia::make_scoped_operation(context_->operation_scope_, run(context_));
}

template <typename t_type>
inline const t_type& context_request::validated() const {
    return context_->request_bindings().get_validated<t_type>();
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename t_type>
inline request_binding_handle<t_type> bind_validated_model(context& context_value, const t_type& model) {
    return context_value.request_bindings().bind_validated(model);
}

}  // namespace ruvia::detail
