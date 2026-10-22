#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/context.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"

namespace ruvia::detail {

template <typename member_type>
struct context_task_result {
    static constexpr bool ok = false;
    using type = void;
};

template <typename controller_type, typename result_type>
struct context_task_result<task<result_type> (controller_type::*)(context&)> {
    static constexpr bool ok = true;
    using type = result_type;
};

// Unevaluated probe used by RUVIA_GET_WS / RUVIA_CONNECT / RUVIA_CONNECT_PROTOCOL
// to distinguish a leading route-config expression `(config) + probe` (binary +)
// from a leading middleware type `(M) + probe` (C-style cast of +probe to M).
struct route_arg_probe final {
    [[nodiscard]] constexpr route_arg_probe operator+() const noexcept {
        return {};
    }

    template <typename target_type>
        requires(!std::same_as<std::remove_cvref_t<target_type>, websocket_route_config> &&
                 !std::same_as<std::remove_cvref_t<target_type>, http_tunnel_route_config>)
    [[nodiscard]] constexpr operator target_type() const noexcept;
};

[[nodiscard]] inline websocket_route_config operator+(
    websocket_route_config config, route_arg_probe) noexcept {
    return config;
}

[[nodiscard]] inline constexpr http_tunnel_route_config operator+(
    http_tunnel_route_config config, route_arg_probe) noexcept {
    return config;
}

template <typename controller_type>
class controller_registration_access final {
    friend controller_type;

    template <typename t_type>
    friend void register_controller_routes(void* controller_value, router& router_value);

    using middleware_list_type = std::pmr::vector<controller_middleware_descriptor>;

    [[nodiscard]] static constexpr std::string_view group_prefix() noexcept {
        if constexpr (requires { controller_type::ruvia_controller_group_prefix(); }) {
            return controller_type::ruvia_controller_group_prefix();
        } else {
            return {};
        }
    }

    [[nodiscard]] static middleware_list_type group_middlewares() {
        if constexpr (requires { controller_type::ruvia_controller_group_middlewares(); }) {
            return controller_type::ruvia_controller_group_middlewares();
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
        std::string_view path, route_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, path, handler, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, path, handler, middlewares);
    }

    static void add_extension_method_route(const controller_route_builder& scope,
        std::string_view method_token, std::string_view path, route_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_extension_method_route(method_token, path, handler, middlewares);
    }

    static void add_extension_method_route(const controller_route_builder& scope,
        std::string_view method_token, std::string_view path, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_extension_method_route(method_token, path, handler, middlewares);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol,
        std::string_view target, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol,
        std::string_view target, route_stream_handler_type handler, http_tunnel_route_config config,
        std::span<const controller_middleware_descriptor> middlewares = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, route_stream_handler_type handler,
        std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config = {}) {
        scope.register_websocket_route(
            method, path, handler, middlewares, std::move(websocket_config));
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, route_stream_handler_type handler,
        websocket_route_config websocket_config,
        std::span<const controller_middleware_descriptor> middlewares = {}) {
        scope.register_websocket_route(
            method, path, handler, middlewares, std::move(websocket_config));
    }

    template <auto handler>
    [[nodiscard]] static auto bind(controller_type* instance) noexcept {
        using result_type = typename context_task_result<decltype(handler)>::type;
        static_assert(context_task_result<decltype(handler)>::ok,
            "handler must take context& and return task<http_response> or task<void>");
        static_assert(std::is_same_v<result_type, http_response> || std::is_same_v<result_type, void>,
            "handlers must return task<http_response> or task<void>; serialize response models with c.json(model)");
        if constexpr (std::is_same_v<result_type, void>) {
            return callback_access::make<task<void>(context&)>(instance, &invoke_stream<handler>);
        } else {
            return callback_access::make<task<http_response>(context&)>(instance, &invoke<handler>);
        }
    }

    template <task<void> (controller_type::*handler)(context&)>
    [[nodiscard]] static route_stream_handler_type bind_stream(controller_type* instance) noexcept {
        return callback_access::make<task<void>(context&)>(instance, &invoke_stream<handler>);
    }

    template <middleware... middleware_types>
    [[nodiscard]] static middleware_list_type make_middlewares() {
        middleware_list_type middlewares(registration_resource());
        if constexpr (sizeof...(middleware_types) > 0) {
            middlewares.reserve(sizeof...(middleware_types));
            (middlewares.push_back(make_middleware_descriptor<middleware_types>()), ...);
        }
        return middlewares;
    }

    static void register_routes(controller_type& controller_value, router& router_value) {
        controller_value.register_routes(router_value);
    }

    template <auto handler>
    [[nodiscard]] static task<http_response> invoke(void* target, context& context_value) {
        return (static_cast<controller_type*>(target)->*handler)(context_value);
    }

    template <task<void> (controller_type::*handler)(context&)>
    [[nodiscard]] static task<void> invoke_stream(void* target, context& context_value) {
        return (static_cast<controller_type*>(target)->*handler)(context_value);
    }
};

template <typename controller_type>
void* create_controller(controller_store& controllers) {
    return &controllers.emplace<controller_type>();
}

template <typename controller_type>
void register_controller_routes(void* controller_value, router& router_value) {
    controller_registration_access<controller_type>::register_routes(
        *static_cast<controller_type*>(controller_value), router_value);
}

template <typename controller_type>
[[nodiscard]] bool register_controller() {
    static_assert(std::is_final_v<controller_type>, "controller must be final");
    static_assert(
        std::is_default_constructible_v<controller_type>, "controller must be default constructible");

    return add_controller_registrar(
        {.create_ = &create_controller<controller_type>, .register_routes_ = &register_controller_routes<controller_type>});
}

}  // namespace ruvia::detail
