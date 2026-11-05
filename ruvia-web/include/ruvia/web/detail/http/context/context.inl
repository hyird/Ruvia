#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/detail/model/parse/form_parser.h"
#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

namespace ruvia {

template <typename t_type>
inline request_state_binding<t_type> context::bind_request_state(const t_type& value) {
    return request_bindings().bind_state(value);
}

template <typename t_type>
inline const std::remove_cvref_t<t_type>& context::request_state() const {
    return request_bindings().get_state<t_type>();
}

template <typename t_type>
    requires detail::is_model_json_value<t_type>
inline http_response context::json(const t_type& value) {
    auto body = to_json(value, {.resource_ = arena()});
    return json_serialized(body);
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::query(std::string_view name) const {
    const auto raw = query(name);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const auto parsed_value = parse_integer<t_type>(*raw);
    if (parsed_value.index() != 0) {
        detail::throw_invalid_query();
    }
    return std::get<0>(parsed_value);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::query(std::string_view name) const {
    const auto raw = query(name);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const auto parsed_value = detail::parse_form_bool(*raw);
    if (!parsed_value.has_value()) {
        detail::throw_invalid_query();
    }
    return *parsed_value;
}

template <detail::parsed_integer t_type>
inline std::optional<t_type> context_request::param(std::string_view name) const {
    const auto raw = param(name);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const auto parsed_value = parse_integer<t_type>(*raw);
    if (parsed_value.index() != 0) {
        detail::throw_invalid_param();
    }
    return std::get<0>(parsed_value);
}

template <std::same_as<bool> t_type>
inline std::optional<bool> context_request::param(std::string_view name) const {
    const auto raw = param(name);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    const auto parsed_value = detail::parse_form_bool(*raw);
    if (!parsed_value.has_value()) {
        detail::throw_invalid_param();
    }
    return *parsed_value;
}

namespace detail {

template <validation_target target>
inline constexpr std::string_view request_body_media_type =
    target == validation_target::json ? "application/json" : "application/x-www-form-urlencoded";

// Whether Content-Type names the media type `target` parses; parameters such
// as charset do not take part in the match.
template <validation_target target>
[[nodiscard]] bool request_has_body_media_type(const context& context_value) {
    const auto content_type = context_value.req().header("Content-Type").value_or(std::string_view{});
    return http_ascii_equals_ignore_case(::ruvia::http_media_type_only(content_type), request_body_media_type<target>);
}

template <validation_target target>
[[noreturn]] inline void throw_invalid_content_type() {
    if constexpr (target == validation_target::json) {
        detail::throw_invalid_json_content_type();
    } else {
        detail::throw_invalid_form_content_type();
    }
}

template <validation_target target, typename model_type>
[[nodiscard]] model_type parse_request_body_model(
    std::string_view raw_body, std::pmr::memory_resource* arena) {
    if constexpr (target == validation_target::json) {
        auto parsed_value = parse_complete_json_value<model_type>(raw_body, arena, model_string_storage::borrowed);
        if (!parsed_value) {
            // Once Content-Type selects JSON, malformed JSON is a 400 rather than
            // an absent optional format. This keeps json_if<T>() from turning an
            // explicitly JSON request into a successful fallback.
            detail::throw_invalid_json_body();
        }
        return std::move(*parsed_value);
    } else if constexpr (target == validation_target::form) {
        return model_parse_access::parse_form<model_type>(form_body_input{.body_ = raw_body}, arena);
    } else {
        static_assert(always_false<model_type>, "unsupported request body validation target");
    }
}

template <validation_target target, typename model_type>
[[nodiscard]] model_type parse_and_validate_request_model(
    std::string_view raw_body, std::pmr::memory_resource* arena) {
    auto model = parse_request_body_model<target, model_type>(raw_body, arena);
    validator validator_value;
    detail::model_validation_access::validate_model(model, validator_value);
    std::move(validator_value).throw_if_invalid();
    return model;
}

template <validation_target target, typename model_type>
[[nodiscard]] task<std::optional<model_type>> parse_optional_request_model(const context& context_value) {
    if (!request_has_body_media_type<target>(context_value)) {
        co_return std::nullopt;
    }
    const auto request_body = co_await context_value.req().text();
    co_return parse_and_validate_request_model<target, model_type>(request_body, context_value.arena());
}

}  // namespace detail

template <typename t_type>
task<std::optional<t_type>> context_request::json_if() const {
    static_assert(detail::is_model<t_type>, "JSON body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(
        context_->operation_scope_, detail::parse_optional_request_model<validation_target::json, t_type>(*context_));
}

template <typename t_type>
task<std::optional<t_type>> context_request::form_if() const {
    static_assert(detail::is_model<t_type>, "form body type must use RUVIA_MODEL");
    return ::ruvia::make_scoped_operation(
        context_->operation_scope_, detail::parse_optional_request_model<validation_target::form, t_type>(*context_));
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

template <validation_target target>
[[nodiscard]] const request_name_value_list& validation_target_fields(context& c) {
    if constexpr (target == validation_target::query) {
        return c.req().query_fields();
    } else if constexpr (target == validation_target::param) {
        return c.req().param_fields();
    } else if constexpr (target == validation_target::header) {
        return c.req().header_fields();
    } else if constexpr (target == validation_target::cookie) {
        return c.req().cookie_fields();
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>,
            "unsupported validator target");
    }
}

template <validation_target target, typename body_type>
task<void> model_validation_middleware<target, body_type>::handle(context& c, next& next_value) {
    std::optional<body_type> parsed_body;
    if constexpr (target == validation_target::json || target == validation_target::form) {
        if (!request_has_body_media_type<target>(c)) {
            throw_invalid_content_type<target>();
        }
        const auto request_body = co_await c.req().text();
        parsed_body.emplace(parse_request_body_model<target, body_type>(request_body, c.arena()));
    } else {
        parsed_body.emplace(model_parse_access::parse_form<body_type>(
            std::cref(validation_target_fields<target>(c)), c.arena()));
    }
    auto& body = *parsed_body;
    validator validator_value;
    validate(body, validator_value);
    std::move(validator_value).throw_if_invalid();
    auto binding = detail::bind_validated_model(c, body);
    co_await next_value();
}

}  // namespace ruvia::detail
