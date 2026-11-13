#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/context.h"
// Internal startup-time controller registration contracts.

#include <memory>
#include <string>

#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/validation.h"
#include "ruvia/web/websocket.h"

namespace ruvia::detail {

class router;

template <typename controller_type>
class controller_registration_access;

class controller_store;

// One registered controller type: how a worker constructs its instance, and
// how that instance declares its routes. Routes are declared once, into the
// process route plan; every worker afterwards only constructs instances.
struct controller_registrar final {
    void* (*create_)(controller_store&){nullptr};
    void (*register_routes_)(void* controller, router&){nullptr};

    [[nodiscard]] friend bool operator==(
        const controller_registrar&, const controller_registrar&) noexcept = default;
};

// The controller instances of one worker, one per registrar in registrar
// order, which is the order the route plan's controller slots use.
class controller_store final {
public:
    controller_store();
    explicit controller_store(std::span<const controller_registrar> registrars);
    ~controller_store();

    controller_store(const controller_store&) = delete;
    controller_store& operator=(const controller_store&) = delete;
    controller_store(controller_store&&) noexcept;
    controller_store& operator=(controller_store&&) noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] void* instance(std::size_t slot) const noexcept;

private:
    template <typename controller_type>
    friend void* create_controller(controller_store& controllers);

    template <typename t_type>
    t_type& emplace() {
        auto* raw = new t_type();
        try {
            add_lifetime(raw, &controller_store::destroy<t_type>);
        } catch (...) {
            delete raw;
            throw;
        }
        return *raw;
    }

    using destroy_type = void (*)(void*) noexcept;

    struct controller_lifetime final {
        void* target_{nullptr};
        destroy_type destroy_{nullptr};

        controller_lifetime(void* target_value, destroy_type destroy_value) noexcept
            : target_(target_value),
              destroy_(destroy_value) {}
        controller_lifetime(const controller_lifetime&) = delete;
        controller_lifetime& operator=(const controller_lifetime&) = delete;
        controller_lifetime(controller_lifetime&& other) noexcept
            : target_(std::exchange(other.target_, nullptr)),
              destroy_(std::exchange(other.destroy_, nullptr)) {}
        void reset() noexcept {
            auto* const target = std::exchange(target_, nullptr);
            auto* const destroy = std::exchange(destroy_, nullptr);
            if (target != nullptr && destroy != nullptr) {
                destroy(target);
            }
        }
        controller_lifetime& operator=(controller_lifetime&& other) noexcept {
            if (this != &other) {
                reset();
                target_ = std::exchange(other.target_, nullptr);
                destroy_ = std::exchange(other.destroy_, nullptr);
            }
            return *this;
        }
        ~controller_lifetime() {
            reset();
        }
    };

    struct controller_lifetime_list final {
        std::vector<controller_lifetime> items_{};

        controller_lifetime_list() = default;
        controller_lifetime_list(const controller_lifetime_list&) = delete;
        controller_lifetime_list& operator=(const controller_lifetime_list&) = delete;
        controller_lifetime_list(controller_lifetime_list&&) noexcept = default;
        controller_lifetime_list& operator=(controller_lifetime_list&& other) noexcept {
            if (this != &other) {
                clear_reverse();
                items_ = std::move(other.items_);
            }
            return *this;
        }
        ~controller_lifetime_list() {
            clear_reverse();
        }
        void clear_reverse() noexcept {
            while (!items_.empty()) {
                items_.pop_back();
            }
        }
        void reserve(std::size_t count) {
            items_.reserve(count);
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return items_.size();
        }
        [[nodiscard]] const controller_lifetime& operator[](std::size_t index) const noexcept {
            return items_[index];
        }
        template <typename... args_type>
        void emplace_back(args_type&&... args) {
            items_.emplace_back(std::forward<args_type>(args)...);
        }
    };

    void add_lifetime(void* target, destroy_type destroy);

    template <typename t_type>
    static void destroy(void* target) noexcept {
        delete static_cast<t_type*>(target);
    }

    controller_lifetime_list lifetimes_{};
};

class controller_route_builder final {
public:
    controller_route_builder(const controller_route_builder&) = delete;
    controller_route_builder& operator=(const controller_route_builder&) = delete;
    ~controller_route_builder();

private:
    template <typename controller_type>
    friend class controller_registration_access;

    controller_route_builder(
        router& router_value, std::string_view prefix, std::vector<controller_middleware_descriptor> middlewares);
    // Each builds the endpoint its route kind runs and registers it through
    // the router's single registration path.
    void register_route(http_known_method method, std::string_view method_token, std::string_view path,
        route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_route(http_known_method method, std::string_view method_token, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const;
    void register_websocket_route(http_known_method method, std::string_view path,
        route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config) const;
    [[nodiscard]] controller_route_builder create_scope(
        std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const;

    struct owned_prefix_tag_type final {};
    controller_route_builder(router& router_value, std::string prefix,
        std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type);

    router& router_;
    std::string prefix_;
    std::vector<controller_middleware_descriptor> middlewares_;
};

[[nodiscard]] bool add_controller_registrar(controller_registrar registrar_value);
// Static controller discovery is complete before main. The first application/test_app
// build seals this registry; loading a controller-bearing module afterwards is
// a startup contract error instead of silently changing only later workers.
[[nodiscard]] std::vector<controller_registrar> seal_controller_registrars();

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

template <typename controller_type, typename result_type>
struct context_task_result<task<result_type> (controller_type::*)(context&) const> {
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

    using middleware_list_type = std::vector<controller_middleware_descriptor>;

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
        std::string_view method_token, std::string_view path, route_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method,
        std::string_view method_token, std::string_view path, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol,
        std::string_view target, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method,
        std::string_view path, route_stream_handler handler,
        std::span<const controller_middleware_descriptor> middlewares,
        websocket_route_config websocket_config = {}) {
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

    template <auto handler>
    [[nodiscard]] static route_stream_handler bind_stream(controller_type* instance) noexcept {
        using result_type = typename context_task_result<decltype(handler)>::type;
        static_assert(context_task_result<decltype(handler)>::ok && std::is_same_v<result_type, void>,
            "stream/upgrade/tunnel handler must take context& and return task<void>");
        return callback_access::make<task<void>(context&)>(instance, &invoke_stream<handler>);
    }

    template <route_modifier... modifier_types>
    [[nodiscard]] static middleware_list_type make_middlewares() {
        middleware_list_type middlewares;
        if constexpr (sizeof...(modifier_types) > 0) {
            middlewares.reserve(sizeof...(modifier_types));
            (middlewares.push_back(make_route_modifier_descriptor<modifier_types>()), ...);
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

    template <auto handler>
    [[nodiscard]] static task<void> invoke_stream(void* target, context& context_value) {
        return (static_cast<controller_type*>(target)->*handler)(context_value);
    }
};

// Methods covered by RUVIA_ALL. HEAD is intentionally omitted: buffered and
// response-stream (including SSE) GET routes provide an implicit HEAD fallback
// without another route record (RFC 9110 9.3.2: same handler and head fields,
// body suppressed); websocket GET routes require an explicit HEAD route.
inline constexpr std::array ruvia_all_route_methods = {http_known_method::get, http_known_method::post,
    http_known_method::put, http_known_method::patch, http_known_method::delete_value,
    http_known_method::options};

// Startup-only holder for the RUVIA_ON method list; the macro pastes the
// parenthesized list as a constructor call.
template <std::size_t n>
class ruvia_method_list final {
public:
    template <std::same_as<http_known_method>... methods_type>
    constexpr explicit ruvia_method_list(methods_type... methods) noexcept
        : methods_{methods...} {
        static_assert(sizeof...(methods_type) > 0, "RUVIA_ON requires at least one method");
        static_assert(sizeof...(methods_type) == n);
    }

    [[nodiscard]] constexpr const http_known_method* begin() const& noexcept {
        return methods_.data();
    }
    [[nodiscard]] constexpr const http_known_method* begin() const&& = delete;

    [[nodiscard]] constexpr const http_known_method* end() const& noexcept {
        return methods_.data() + n;
    }
    [[nodiscard]] constexpr const http_known_method* end() const&& = delete;

private:
    std::array<http_known_method, n> methods_{};
};

template <typename... methods_type>
ruvia_method_list(methods_type...) -> ruvia_method_list<sizeof...(methods_type)>;

// Startup-only holder for the RUVIA_ON path list; the macro pastes the
// parenthesized list as a constructor call.
template <std::size_t n>
class ruvia_path_list final {
public:
    template <typename... paths_type>
        requires(std::constructible_from<borrowed_text, paths_type&&> && ...)
    constexpr explicit ruvia_path_list(paths_type&&... paths) noexcept
        : paths_{borrowed_text(std::forward<paths_type>(paths)).view()...} {
        static_assert(sizeof...(paths_type) > 0, "RUVIA_ON requires at least one path");
        static_assert(sizeof...(paths_type) == n);
    }

    template <typename... paths_type>
        requires((std::convertible_to<paths_type&&, std::string_view> && ...) &&
                    (!std::constructible_from<borrowed_text, paths_type&&> || ...))
    explicit ruvia_path_list(paths_type&&...) = delete;

    [[nodiscard]] constexpr const std::string_view* begin() const& noexcept {
        return paths_.data();
    }
    [[nodiscard]] constexpr const std::string_view* begin() const&& = delete;

    [[nodiscard]] constexpr const std::string_view* end() const& noexcept {
        return paths_.data() + n;
    }
    [[nodiscard]] constexpr const std::string_view* end() const&& = delete;

private:
    std::array<std::string_view, n> paths_{};
};

template <typename... paths_type>
ruvia_path_list(paths_type&&...) -> ruvia_path_list<sizeof...(paths_type)>;

}  // namespace ruvia::detail

#define RUVIA_CONTROLLER_GROUP(prefix, ...)                                                      \
private:                                                                                         \
    [[nodiscard]] static constexpr ::std::string_view ruvia_controller_group_prefix() noexcept { \
        static_assert(::std::constructible_from<::ruvia::borrowed_text, decltype((prefix))>,     \
            "controller group prefixes must outlive route registration");                        \
        return ::ruvia::borrowed_text(prefix).view();                                            \
    }                                                                                            \
    [[nodiscard]] static auto ruvia_controller_group_middlewares() {                             \
        return ::ruvia::detail::controller_registration_access<                                  \
            ruvia_controller_type>::template make_middlewares<__VA_ARGS__>();                    \
    }

// A controller is a final, default-constructible class that derives from
// nothing. RUVIA_ROUTES_BEGIN names it because the registration flag
// RUVIA_ROUTES_END declares is a static data member initializer, which is not a
// complete-class context and so cannot deduce the enclosing type.
#define RUVIA_ROUTES_BEGIN(controller_type)                                                                   \
private:                                                                                                      \
    using ruvia_controller_type = controller_type;                                                            \
    using ruvia_controller_access_type =                                                                      \
        ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                               \
    friend class ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                      \
    void register_routes(::ruvia::detail::router& router) {                                                   \
        static_assert(::std::is_same_v<ruvia_controller_type, ::std::remove_cvref_t<decltype(*this)>>,        \
            "RUVIA_ROUTES_BEGIN must name its enclosing controller");                                         \
        auto ruvia_controller_group = ruvia_controller_access_type::create_route_group(router,                \
            ruvia_controller_access_type::group_prefix(), ruvia_controller_access_type::group_middlewares()); \
        [[maybe_unused]] auto& ruvia_route_scope = ruvia_controller_group;

#if defined(_MSC_VER)
// selectany is invalid when a controller has internal linkage (for example in
// an anonymous namespace). MSVC already retains dynamic inline-variable
// initialization, so no additional declaration attribute is needed here.
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#elif defined(__GNUC__) || defined(__clang__)
#define RUVIA_DETAIL_CONTROLLER_RETAIN __attribute__((used))
#else
#define RUVIA_DETAIL_CONTROLLER_RETAIN
#endif

#define RUVIA_ROUTES_END                                                                   \
    }                                                                                      \
    RUVIA_DETAIL_CONTROLLER_RETAIN inline static const bool ruvia_controller_registered_ = \
        ::ruvia::detail::register_controller<ruvia_controller_type>();

#define RUVIA_DETAIL_ADD_METHOD_ROUTE(method, path, handler, ...)                           \
    ruvia_controller_access_type::add_route(ruvia_route_scope, method, {}, path,            \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Route paths and request paths compare in one canonical form
// (ruvia::uri_paths_equivalent): segments split on literal '/', each compared
// by its percent-decoded octets, so "/users/%61dmin" reaches the "/users/admin"
// route and its middleware, hex case never matters, and "%2F" stays inside its
// segment. Path-scoped middleware, scoped fallbacks and decoded route
// parameters use the same form; two patterns differing only in spelling are a
// duplicate registration. A GET route also answers HEAD unless it is a
// websocket route (see ruvia_all_route_methods).
#define RUVIA_GET(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::get, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_SELECT_SECOND(first, second, ...) second
#define RUVIA_DETAIL_CHOOSE_VA_MACRO(with_args, without_args, ...) \
    RUVIA_DETAIL_SELECT_SECOND(__VA_OPT__(, ) with_args, without_args)

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS(protocol, target, handler)                           \
    ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
        ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
        ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS(protocol, target, handler, first, ...)                     \
    [&](auto ruvia_route_probe) {                                                                          \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0,                  \
            ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                           \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::http_tunnel_route_config>) {         \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(),                    \
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));                           \
        } else {                                                                                           \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target,            \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<                                   \
                    ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>());                                   \
        }                                                                                                  \
    }(::ruvia::detail::route_arg_probe{})

// Ordinary CONNECT uses an exact host:port authority or "*". The only
// upgrade protocol is "connect-udp"; any other fails registration with
// std::invalid_argument. Group prefixes apply only to upgrade-protocol tunnel
// paths; controller/group middleware always runs.
#define RUVIA_CONNECT(authority, handler, ...)                            \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)({}, authority, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_CONNECT_PROTOCOL(protocol, path, handler, ...)              \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)(protocol, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS(path, handler)                                                  \
    ruvia_controller_access_type::add_websocket_route(ruvia_route_scope, ::ruvia::http_known_method::get, \
        path, ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this),  \
        ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS(path, handler, first, ...)                                     \
    [&](auto ruvia_route_probe) {                                                                          \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0,                  \
            ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                           \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::websocket_route_config>) {           \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope,                           \
                ::ruvia::http_known_method::get, path,                                                     \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(),                    \
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));                           \
        } else {                                                                                           \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope,                           \
                ::ruvia::http_known_method::get, path,                                                     \
                ruvia_controller_access_type::template bind_stream<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<                                   \
                    ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>());                                   \
        }                                                                                                  \
    }(::ruvia::detail::route_arg_probe{})

#define RUVIA_GET_WS(path, handler, ...)                              \
    RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS, \
        RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS, __VA_ARGS__)(path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_POST(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::post, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PUT(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::put, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DELETE(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::delete_value, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PATCH(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::patch, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_HEAD(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::head, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_OPTIONS(path, handler, ...) \
    RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::options, path, handler __VA_OPT__(, ) __VA_ARGS__)

// An extension method, routed by its exact wire token: PROPFIND, PURGE, LOCK.
// HTTP's method space is open (http_known_method is only this framework's fixed
// classification, never the wire value), but routes were keyed on that enum, so
// anything outside it answered 501 and could not be served at all.
//
// The token is case-sensitive per RFC 9110 9.1, must be a valid method token,
// and must NOT be one of the classified methods -- those keep the enum as their
// identity and their own macros. The path must be static; only the classified
// methods have dynamic-segment routes.
#define RUVIA_METHOD(method_token, path, handler, ...)                                              \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::unknown, \
        method_token, path,                                                                         \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),         \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Hono app.all: registers the handler for GET/POST/PUT/PATCH/DELETE/OPTIONS.
#define RUVIA_ALL(path, handler, ...)                                                               \
    {                                                                                               \
        auto&& ruvia_route_path = (path);                                                           \
        for (const auto ruvia_route_method : ::ruvia::detail::ruvia_all_route_methods) {            \
            ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {},      \
                ruvia_route_path,                                                                   \
                ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());            \
        }                                                                                           \
    }

// Hono app.on: registers the handler for an explicit method x path list, e.g.
// RUVIA_ON((ruvia::http_known_method::put, ruvia::http_known_method::delete_value),
//     ("/items/:id", "/legacy/:id"), handler).
#define RUVIA_ON(methods, paths, handler, ...)                                                          \
    {                                                                                                   \
        auto ruvia_route_methods = ::ruvia::detail::ruvia_method_list methods;                          \
        auto ruvia_route_paths = ::ruvia::detail::ruvia_path_list paths;                                \
        for (const auto ruvia_route_method : ruvia_route_methods) {                                     \
            for (const auto ruvia_route_path : ruvia_route_paths) {                                     \
                ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {},      \
                    ruvia_route_path,                                                                   \
                    ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this), \
                    ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());            \
            }                                                                                           \
        }                                                                                               \
    }
#define RUVIA_GROUP_BEGIN(prefix, ...)                                                               \
    {                                                                                                \
        static_assert(::std::constructible_from<::ruvia::borrowed_text, decltype((prefix))>,         \
            "route group prefixes must outlive route registration");                                 \
        auto ruvia_route_group = ruvia_controller_access_type::create_route_group(ruvia_route_scope, \
            ::ruvia::borrowed_text(prefix).view(),                                                   \
            ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());                 \
        auto& ruvia_route_scope = ruvia_route_group;

#define RUVIA_GROUP_END }
