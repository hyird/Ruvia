#pragma once

#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/context.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/detail/model/rule/rules.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/model.h"
#include "ruvia/web/next.h"
#include "ruvia/web/validation.h"

namespace ruvia {

namespace detail {

template <validation_target target>
[[noreturn]] inline void throw_invalid_validation_target() {
    if constexpr (target == validation_target::query) {
        detail::throw_invalid_query();
    } else if constexpr (target == validation_target::param) {
        detail::throw_invalid_param();
    } else if constexpr (target == validation_target::header) {
        detail::throw_invalid_header();
    } else if constexpr (target == validation_target::cookie) {
        detail::throw_invalid_cookie();
    } else {
        static_assert(always_false<std::integral_constant<validation_target, target>>,
            "unsupported validator target");
    }
}

template <validation_target target, typename body_t_type>
[[nodiscard]] body_t_type parse_validated_fields(context& c, const request_name_value_list& fields_value) {
    static_assert(is_model<body_t_type>, "field validator body type must use RUVIA_MODEL");
    auto parsed_value = detail::model_parse_access::parse_form_fields_partial<body_t_type>(fields_value, c.arena());
    if (!parsed_value) {
        throw_invalid_validation_target<target>();
    }
    return std::move(*parsed_value);
}

template <validation_target target, typename body_t_type>
[[nodiscard]] body_t_type parse_validated_fields(context& c) {
    if constexpr (target == validation_target::query) {
        return parse_validated_fields<target, body_t_type>(c, c.req().query_fields());
    } else if constexpr (target == validation_target::param) {
        return parse_validated_fields<target, body_t_type>(c, c.req().param_fields());
    } else if constexpr (target == validation_target::header) {
        return parse_validated_fields<target, body_t_type>(c, c.req().header_fields());
    } else if constexpr (target == validation_target::cookie) {
        return parse_validated_fields<target, body_t_type>(c, c.req().cookie_fields());
    } else {
        static_assert(always_false<body_t_type>, "unsupported validator target");
    }
}

// One validating middleware per request source: parses the model from `target`,
// runs its declared rules, binds it for the handler, then continues the chain.
template <validation_target target, typename body_t_type>
class model_validation_middleware final {
public:
    static_assert(is_model<body_t_type>, "request validation middleware requires a RUVIA_MODEL");
    static_assert(target != validation_target::header ||
                      model::unique_model_wire_names_ignore_ascii_case(model::model_access::schema<body_t_type>()),
        "header_model field names must be unique ignoring ASCII case");
    using ruvia_validation_body_type = body_t_type;

    void validate(const body_t_type& body, validator& validator_value) const {
        model_validation_access::validate_model(body, validator_value);
    }

    [[nodiscard]] task<void> handle(context& c, next& next_value) {
        if constexpr (target == validation_target::json) {
            if (!detail::content_type_matches(
                    c.req().header("Content-Type").value_or(std::string_view{}), "application/json")) {
                detail::throw_invalid_json_content_type();
            }
            const auto raw_json = co_await c.req().text();
            auto parsed_value =
                detail::model_parse_access::parse_json_borrowed_partial<body_t_type>(raw_json, c.arena());
            if (!parsed_value) {
                detail::throw_invalid_json_body();
            }
            body_t_type body = std::move(*parsed_value);
            validator validator_value({.resource_ = c.arena()});
            validate(body, validator_value);
            validator_value.throw_if_invalid();
            auto binding = bind_validated_json_model(c, body, raw_json);
            co_await next_value();
        } else if constexpr (target == validation_target::form) {
            if (!detail::content_type_matches(c.req().header("Content-Type").value_or(std::string_view{}),
                    "application/x-www-form-urlencoded")) {
                detail::throw_invalid_form_content_type();
            }
            const auto request_body = co_await c.req().text();
            auto parsed_value =
                detail::model_parse_access::parse_form_borrowed_partial<body_t_type>(request_body, c.arena());
            if (!parsed_value) {
                detail::throw_invalid_form_body();
            }
            body_t_type body = std::move(*parsed_value);
            validator validator_value({.resource_ = c.arena()});
            validate(body, validator_value);
            validator_value.throw_if_invalid();
            auto binding = bind_validated_model(c, body);
            co_await next_value();
        } else {
            body_t_type body = parse_validated_fields<target, body_t_type>(c);
            validator validator_value({.resource_ = c.arena()});
            validate(body, validator_value);
            validator_value.throw_if_invalid();
            auto binding = bind_validated_model(c, body);
            co_await next_value();
        }
    }
};

}  // namespace detail

template <typename body_t_type>
using json_body = detail::model_validation_middleware<validation_target::json, body_t_type>;

template <typename body_t_type>
using form_body = detail::model_validation_middleware<validation_target::form, body_t_type>;

template <typename body_t_type>
using query_model = detail::model_validation_middleware<validation_target::query, body_t_type>;

template <typename body_t_type>
using path_model = detail::model_validation_middleware<validation_target::param, body_t_type>;

template <typename body_t_type>
using header_model = detail::model_validation_middleware<validation_target::header, body_t_type>;

template <typename body_t_type>
using cookie_model = detail::model_validation_middleware<validation_target::cookie, body_t_type>;

}  // namespace ruvia
