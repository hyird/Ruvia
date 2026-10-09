#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/detail/model/parse/parser.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

namespace ruvia {

namespace detail {

template <typename member_t_type>
struct context_task_result {
    static constexpr bool ok = false;
    using type = void;
};

template <typename controller_t_type, typename result_t_type>
struct context_task_result<task<result_t_type> (controller_t_type::*)(context&)> {
    static constexpr bool ok = true;
    using type = result_t_type;
};

}  // namespace detail

template <typename controller_t_type>
class controller {
public:
    using ruvia_controller_type = controller_t_type;
    constexpr controller() noexcept = default;
    ~controller() = default;
};

namespace detail {

template <typename controller_t_type>
class controller_registration_access final {
    friend controller_t_type;

    template <typename t_type>
    friend void register_controller_instance(router& router_value, controller_store& controller_lifetimes);

    using middleware_list_type = std::pmr::vector<controller_middleware_descriptor>;

    [[nodiscard]] static constexpr std::string_view group_prefix() noexcept {
        if constexpr (requires { controller_t_type::ruvia_controller_group_prefix(); }) {
            return controller_t_type::ruvia_controller_group_prefix();
        } else {
            return {};
        }
    }

    [[nodiscard]] static middleware_list_type group_middlewares() {
        if constexpr (requires { controller_t_type::ruvia_controller_group_middlewares(); }) {
            return controller_t_type::ruvia_controller_group_middlewares();
        } else {
            return make_middlewares<>();
        }
    }

    [[nodiscard]] static controller_route_builder create_route_group(
        router& router_value, std::string_view prefix, middleware_list_type middlewares) {
        return controller_route_builder(router_value, prefix, std::move(middlewares));
    }

    [[nodiscard]] static controller_route_builder create_route_group(
        const controller_route_builder& scope, std::string_view prefix,
        const middleware_list_type& middlewares) {
        return scope.create_scope(prefix, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, controller_route_handler_type handler, request_body_mode body_mode,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, path, handler, body_mode, middlewares);
    }

    static void add_extension_method_route(const controller_route_builder& scope,
        std::string_view method_token, std::string_view path, controller_route_handler_type handler,
        request_body_mode body_mode, std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_extension_method_route(method_token, path, handler, body_mode, middlewares);
    }

    static void add_response_stream_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_response_stream_route(method, path, handler, middlewares);
    }

    static void add_sse_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_sse_route(method, path, handler, middlewares);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol,
        std::string_view target, controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, controller_route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config = {}) {
        scope.register_websocket_route(
            method, path, handler, middlewares, std::move(websocket_config));
    }

    template <auto handler>
    [[nodiscard]] static controller_route_handler_type bind(controller_t_type* instance) noexcept {
        using result_t_type = typename context_task_result<decltype(handler)>::type;
        static_assert(context_task_result<decltype(handler)>::ok,
            "handler must take Context& and return Task<HttpResponse>");
        static_assert(std::is_same_v<result_t_type, http_response>,
            "ordinary handlers must return Task<HttpResponse>; serialize response models with c.json(model)");
        return controller_route_handler_type(instance, &invoke<handler>);
    }

    template <task<void> (controller_t_type::*handler)(context&)>
    [[nodiscard]] static controller_route_stream_handler_type bind_stream(controller_t_type* instance) noexcept {
        return controller_route_stream_handler_type(instance, &invoke_stream<handler>);
    }

    template <typename middleware_t_type>
    [[nodiscard]] static controller_middleware_descriptor make_middleware() {
        return make_middleware_descriptor<middleware_t_type>();
    }

    template <typename... middleware_ts_type>
    [[nodiscard]] static middleware_list_type make_middlewares() {
        middleware_list_type middlewares(registration_resource());
        if constexpr (sizeof...(middleware_ts_type) > 0) {
            middlewares.reserve(sizeof...(middleware_ts_type));
            (middlewares.push_back(make_middleware<middleware_ts_type>()), ...);
        }
        return middlewares;
    }

    static void register_routes(controller_t_type& controller_value, router& router_value) {
        controller_value.register_routes(router_value);
    }

    template <auto handler>
    [[nodiscard]] static task<http_response> invoke(void* target, context& context_value) {
        return (static_cast<controller_t_type*>(target)->*handler)(context_value);
    }

    template <task<void> (controller_t_type::*handler)(context&)>
    [[nodiscard]] static task<void> invoke_stream(void* target, context& context_value) {
        return (static_cast<controller_t_type*>(target)->*handler)(context_value);
    }
};

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
[[nodiscard]] task<body_t_type> parse_validated_body(context& c) {
    if constexpr (target == validation_target::json) {
        if (!detail::content_type_matches(
                c.req().header("Content-Type").value_or(std::string_view{}), "application/json")) {
            detail::throw_invalid_json_content_type();
        }
        const auto request_body = co_await c.req().text();
        auto parsed_value =
            detail::model_parse_access::parse_json_borrowed_partial<body_t_type>(request_body, c.arena());
        if (!parsed_value) {
            detail::throw_invalid_json_body();
        }
        co_return std::move(*parsed_value);
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
        co_return std::move(*parsed_value);
    } else if constexpr (target == validation_target::query) {
        co_return parse_validated_fields<target, body_t_type>(c, c.req().query_fields());
    } else if constexpr (target == validation_target::param) {
        co_return parse_validated_fields<target, body_t_type>(c, c.req().param_fields());
    } else if constexpr (target == validation_target::header) {
        co_return parse_validated_fields<target, body_t_type>(c, c.req().header_fields());
    } else if constexpr (target == validation_target::cookie) {
        co_return parse_validated_fields<target, body_t_type>(c, c.req().cookie_fields());
    } else {
        static_assert(always_false<body_t_type>, "unsupported validator target");
    }
}

template <validation_target target, typename body_t_type, typename validator_t_type>
task<void> invoke_model_validator(const validator_t_type& validator_middleware, context& c, next& next_value) {
    body_t_type body = co_await parse_validated_body<target, body_t_type>(c);
    validator validator_value({.resource_ = c.arena()});
    validator_middleware.validate(body, validator_value);
    std::move(validator_value).throw_if_invalid();
    if constexpr (target == validation_target::json) {
        const auto raw_json = co_await c.req().text();
        auto binding = bind_validated_json_model(c, body, raw_json);
        co_await next_value();
    } else {
        auto binding = bind_validated_model(c, body);
        co_await next_value();
    }
}

template <typename controller_t_type>
void register_controller_instance(router& router_value, controller_store& controller_lifetimes) {
    auto& controller_value = controller_lifetimes.emplace<controller_t_type>();
    controller_registration_access<controller_t_type>::register_routes(controller_value, router_value);
}

template <typename controller_t_type>
[[nodiscard]] bool register_controller() {
    static_assert(std::is_base_of_v<controller<controller_t_type>, controller_t_type>,
        "controller must derive from ruvia::Controller<ControllerT>");
    static_assert(std::is_final_v<controller_t_type>, "controller must be final");
    static_assert(
        std::is_default_constructible_v<controller_t_type>, "controller must be default constructible");

    return add_controller_registrar(&register_controller_instance<controller_t_type>);
}

inline void register_controllers(router& router_value, controller_store& controller_lifetimes,
    std::span<const controller_registrar_type> registrars) {
    run_controller_registrars(router_value, controller_lifetimes, registrars);
}

}  // namespace detail

}  // namespace ruvia
