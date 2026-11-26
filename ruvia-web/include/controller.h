#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "context.h"
#include "function_ref.h"
#include "http_error.h"
#include "http_header.h"
#include "http_message.h"
#include "http_request.h"
#include "http_response.h"
#include "http_tunnel.h"
#include "middleware.h"
#include "model.h"
#include "model_binding.h"
#include "rate_limiting.h"
#include "task.h"
#include "websocket.h"
#include "worker_services.h"

namespace ruvia::detail {

// One canonical spelling is shared by application, test_app and route_table.
// router_impl receives the already-normalized registrations from the public
// configuration facades.
// A trailing slash does not create a different path scope; accepting both
// spellings at one layer and not another makes duplicate fallback handlers
// order-dependent and lets tests exercise a different route graph.
// "/" (or any all-slash spelling) is rejected: it would scope every path and
// silently shadow or duplicate the unscoped registration, which is the only
// app-wide spelling.
[[nodiscard]] std::string_view normalize_fallback_prefix(std::string_view prefix);

class router;
class app_configuration;

template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_scoped_app_middleware(const middleware_scope_options& options, args_types&&... args) {
    static_assert(!middleware_runs_on_unmatched_requests<middleware_type>(),
        "a middleware declaring ruvia_runs_on_unmatched_requests cannot be path-scoped with "
        "use_at(); register it app-wide with use<T>()");
    const auto normalized = normalize_fallback_prefix(options.prefix_.view());
    return make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...).scoped_to(normalized);
}

template <typename state_type>
[[nodiscard]] worker_state_definition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "use_worker_state<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return worker_state_definition::make<state_type>([] {
        return state_type();
    });
}

}  // namespace ruvia::detail

namespace ruvia {

// The dispatch configuration application, test_app and router share, declared
// once: middleware, per-worker state, fallback handlers and the host-wide rate
// limit, all stored in one detail::app_configuration. A host supplies only its
// lifecycle guard -- a stopped application, a test_app before its first
// request(), an unfrozen router on its worker -- and a call that guard refuses
// throws std::logic_error. nullptr clears a setting; a cleared handler is
// destroyed after the host released its guard, so its destructor may reenter
// the host. Non-template members are defined in the library and instantiated
// there for the three hosts.
template <typename host_type>
class dispatch_configuration {
public:
    // Registers per-worker middleware instances in order before the matched
    // route's controller and route middleware. Arguments are retained at
    // registration; route model validators must be declared on their route.
    template <middleware middleware_type, typename... args_types>
    host_type& use(args_types&&... args) & {
        return add_middleware(detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }
    template <middleware middleware_type, typename... args_types>
    host_type& use(args_types&&...) && = delete;

    // Runs the middleware exactly on requests whose path is under the prefix:
    // whole path segments, compared percent-decoded, with trailing slashes
    // normalized ("/admin" covers "/admin", "/admin/x" and "/%61dmin/x", never
    // "/administrator"). Scope follows the request path, not the route
    // pattern: a route "/:section/panel" runs it for "/admin/panel" but not
    // "/other/panel". Membership is compiled into the route plan; only a route
    // that serves paths both inside and outside the scope checks the request
    // path at dispatch. Middleware constructor arguments cannot be confused
    // with the separately named scope. A "/" prefix throws
    // std::invalid_argument: use<T>() is the only host-wide spelling.
    template <middleware middleware_type, typename... args_types>
    host_type& use_at(const middleware_scope_options& options, args_types&&... args) & {
        return add_middleware(detail::make_scoped_app_middleware<middleware_type>(options, std::forward<args_types>(args)...));
    }
    template <middleware middleware_type, typename... args_types>
    host_type& use_at(const middleware_scope_options&, args_types&&...) && = delete;

    // One recipe per type; every worker of the host owns an independent
    // instance. A request or posted job only borrows its own worker's instance.
    template <typename state_type, typename factory_type>
    host_type& use_worker_state(factory_type&& factory) & {
        return add_worker_state(detail::worker_state_definition::make<state_type>(std::forward<factory_type>(factory)));
    }
    template <typename state_type, typename factory_type>
    host_type& use_worker_state(factory_type&&) && = delete;

    template <typename state_type>
    host_type& use_worker_state() & {
        return add_worker_state(detail::make_default_worker_state<state_type>());
    }
    template <typename state_type>
    host_type& use_worker_state() && = delete;

    // The host-wide fallbacks; registering one a second time throws
    // std::invalid_argument instead of silently replacing the first.
    host_type& on_error(http_error_handler handler) &;
    host_type& on_error(http_error_handler handler) && = delete;
    host_type& on_not_found(http_not_found_handler handler) &;
    host_type& on_not_found(http_not_found_handler handler) && = delete;
    host_type& on_error(std::nullptr_t) &;
    host_type& on_error(std::nullptr_t) && = delete;
    host_type& on_not_found(std::nullptr_t) &;
    host_type& on_not_found(std::nullptr_t) && = delete;
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // deepest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix") compared
    // percent-decoded; the prefix-less on_error/not_found remain the host-wide
    // fallback. A trailing slash is ignored; registering an equivalent prefix
    // twice ("/api", "/api/", "/%61pi") throws std::invalid_argument instead
    // of silently choosing by call order, and so does the prefix "/", which
    // would shadow the prefix-less form.
    host_type& on_error(scoped_error_handler_options options) &;
    host_type& on_error(scoped_error_handler_options options) && = delete;
    host_type& on_not_found(scoped_not_found_handler_options options) &;
    host_type& on_not_found(scoped_not_found_handler_options options) && = delete;

    // The rate limit every request of the host passes, matched or not, keyed
    // on the client address; replaces an earlier rule. A route may add its own
    // with ruvia::rate_limit<max, window_ms>; both then apply, so the stricter
    // is what a caller actually meets -- the same "narrower scope may only
    // tighten" rule server_config::max_buffered_body_bytes_ follows.
    //
    // Worker-local: each worker counts independently, so an application with
    // N workers admits up to N times this rule. Size it accordingly.
    host_type& rate_limit(rate_limit_config config) &;
    host_type& rate_limit(rate_limit_config config) && = delete;
    host_type& rate_limit(std::nullptr_t) &;
    host_type& rate_limit(std::nullptr_t) && = delete;

private:
    friend host_type;

    dispatch_configuration() = default;

    host_type& add_middleware(detail::owned_middleware_descriptor descriptor);
    host_type& add_worker_state(detail::worker_state_definition definition);
    // Runs `change` under the host's lifecycle guard: the host's
    // edit_configuration(detail::callback_ref<void(detail::app_configuration&)>).
    template <typename change_type>
    host_type& edit(change_type&& change);
};

}  // namespace ruvia

namespace ruvia::detail {

// Internal startup-time controller registration contracts.

template <typename controller_type>
class controller_registration_access;

// One registered controller type: how a worker constructs and destroys its
// instance, and how that instance declares its routes. Routes are declared
// once, into the process route plan; every worker afterwards only constructs
// instances.
struct controller_registrar final {
    void* (*create_)(){nullptr};
    void (*destroy_)(void* controller) noexcept = nullptr;
    void (*register_routes_)(void* controller, router&){nullptr};

    [[nodiscard]] friend bool operator==(const controller_registrar&, const controller_registrar&) noexcept = default;
};

class controller_route_builder final {
public:
    controller_route_builder(const controller_route_builder&) = delete;
    controller_route_builder& operator=(const controller_route_builder&) = delete;
    ~controller_route_builder();

private:
    template <typename controller_type>
    friend class controller_registration_access;

    controller_route_builder(router& router_value, std::string_view prefix, std::vector<controller_middleware_descriptor> middlewares);
    // Each builds the endpoint its route kind runs and registers it through
    // the router's single registration path.
    void register_route(http_known_method method, std::string_view method_token, std::string_view path, route_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_route(http_known_method method, std::string_view method_token, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) const;
    void register_tunnel_route(std::string_view protocol, std::string_view target, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config) const;
    void register_websocket_route(http_known_method method, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, websocket_route_config websocket_config) const;
    [[nodiscard]] controller_route_builder create_scope(std::string_view prefix, std::span<const controller_middleware_descriptor> middlewares) const;

    struct owned_prefix_tag_type final {};
    controller_route_builder(router& router_value, std::string prefix, std::vector<controller_middleware_descriptor> middlewares, owned_prefix_tag_type);

    router& router_;
    std::string prefix_;
    std::vector<controller_middleware_descriptor> middlewares_;
};

[[nodiscard]] bool add_controller_registrar(controller_registrar registrar_value);

template <typename controller_type>
[[nodiscard]] void* create_controller() {
    return new controller_type();
}

template <typename controller_type>
void destroy_controller(void* controller_value) noexcept {
    delete static_cast<controller_type*>(controller_value);
}

template <typename controller_type>
void register_controller_routes(void* controller_value, router& router_value) {
    controller_registration_access<controller_type>::register_routes(*static_cast<controller_type*>(controller_value), router_value);
}

// The one registrar a controller type contributes, shared by RUVIA_ROUTES_END
// (process registry for application) and test_app::controllers<...>().
template <typename controller_type>
[[nodiscard]] controller_registrar controller_registrar_of() noexcept {
    static_assert(std::is_final_v<controller_type>, "controller must be final");
    static_assert(std::is_default_constructible_v<controller_type>, "controller must be default constructible");

    return {.create_ = &create_controller<controller_type>, .destroy_ = &destroy_controller<controller_type>, .register_routes_ = &register_controller_routes<controller_type>};
}

// Only route and enclosing group validators supply typed arguments. Each
// middleware owns its validated value until its downstream task completes.
template <typename... model_types>
struct route_model_arguments final {
    template <typename... inner_types>
    constexpr auto operator+(route_model_arguments<inner_types...>) const noexcept {
        return route_model_arguments<model_types..., inner_types...>{};
    }

    template <typename owner_type, typename result_type>
    static auto signature(task<result_type> (owner_type::*)(context&, const model_types&...)) -> task<result_type>;
    template <typename owner_type, typename result_type>
    static auto signature(task<result_type> (owner_type::*)(context&, const model_types&...) const) -> task<result_type>;

    template <typename callback, typename... target_type>
    static decltype(auto) invoke(callback&& fn, context& c, target_type&&... target) {
        return std::invoke(std::forward<callback>(fn), std::forward<target_type>(target)..., c, c.request_bindings().template get_validated<model_types>()...);
    }
};

template <typename modifier_type>
constexpr auto modifier_models(std::type_identity<modifier_type>) noexcept {
    return route_model_arguments<>{};
}

template <validation_target target, typename model_type, bool strict, std::uint16_t status>
constexpr auto modifier_models(std::type_identity<model_validation_middleware<target, model_type, strict, status>>) noexcept {
    return route_model_arguments<model_type>{};
}

template <route_modifier... modifier_types>
constexpr auto route_models() noexcept {
    return (route_model_arguments<>{} + ... + modifier_models(std::type_identity<modifier_types>{}));
}

// Unevaluated probe used by RUVIA_GET_WS / RUVIA_CONNECT / RUVIA_CONNECT_PROTOCOL
// to distinguish a leading route-config expression `(config) + probe` (binary +)
// from a leading middleware type `(M) + probe` (C-style cast of +probe to M).
struct route_arg_probe final {
    [[nodiscard]] constexpr route_arg_probe operator+() const noexcept {
        return {};
    }

    template <typename target_type>
        requires(!std::same_as<std::remove_cvref_t<target_type>, websocket_route_config> && !std::same_as<std::remove_cvref_t<target_type>, http_tunnel_route_config>)
    [[nodiscard]] constexpr operator target_type() const noexcept;
};

[[nodiscard]] inline websocket_route_config operator+(websocket_route_config config, route_arg_probe) noexcept {
    return config;
}

[[nodiscard]] inline constexpr http_tunnel_route_config operator+(http_tunnel_route_config config, route_arg_probe) noexcept {
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

    static constexpr auto group_models() noexcept {
        if constexpr (requires { controller_type::ruvia_controller_group_models(); }) {
            return controller_type::ruvia_controller_group_models();
        } else {
            return route_model_arguments<>{};
        }
    }

    [[nodiscard]] static middleware_list_type group_middlewares() {
        if constexpr (requires { controller_type::ruvia_controller_group_middlewares(); }) {
            return controller_type::ruvia_controller_group_middlewares();
        } else {
            return make_middlewares<>();
        }
    }

    [[nodiscard]] static controller_route_builder create_route_group(router& router_value, std::string_view prefix, middleware_list_type middlewares) {
        return controller_route_builder(router_value, prefix, std::move(middlewares));
    }

    template <route_modifier... modifiers, typename outer_models, typename declare_routes>
    static void with_route_group(const controller_route_builder& parent, borrowed_text prefix, outer_models outer, declare_routes&& declare) {
        const auto group = parent.create_scope(prefix.view(), make_middlewares<modifiers...>());
        declare(group, outer + route_models<modifiers...>());
    }

    static void add_route(const controller_route_builder& scope, http_known_method method, std::string_view method_token, std::string_view path, route_handler handler, std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_route(const controller_route_builder& scope, http_known_method method, std::string_view method_token, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares) {
        scope.register_route(method, method_token, path, handler, middlewares);
    }

    static void add_tunnel_route(const controller_route_builder& scope, std::string_view protocol, std::string_view target, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, http_tunnel_route_config config = {}) {
        scope.register_tunnel_route(protocol, target, handler, middlewares, config);
    }

    static void add_websocket_route(const controller_route_builder& scope, http_known_method method, std::string_view path, route_stream_handler handler, std::span<const controller_middleware_descriptor> middlewares, websocket_route_config websocket_config = {}) {
        scope.register_websocket_route(method, path, handler, middlewares, std::move(websocket_config));
    }

    template <auto handler, typename models>
    [[nodiscard]] static auto bind(controller_type* instance, models) noexcept {
        if constexpr (requires { models::signature(handler); }) {
            using result = decltype(models::signature(handler));
            using erased_handler = std::conditional_t<std::same_as<result, task<void>>, route_stream_handler, route_handler>;
            static_assert(std::same_as<result, task<void>> || std::same_as<result, task<http_response>>, "route handler must return task<http_response> or task<void>");
            return erased_handler::from_raw(instance, [](void* target, context& c) {
                return models::invoke(handler, c, static_cast<controller_type*>(target));
            });
        } else if constexpr (!std::same_as<models, route_model_arguments<>> && requires { route_model_arguments<>::signature(handler); }) {
            // The route can enforce validation without consuming its result.
            return bind<handler>(instance, route_model_arguments<>{});
        } else {
            static_assert(always_false<models>, "route handler must take context& followed by const references to its declared validated models in order, or context& alone");
        }
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
};

// Methods covered by RUVIA_ALL. HEAD is intentionally omitted: buffered and
// response-stream (including SSE) GET routes provide an implicit HEAD fallback
// without another route record (RFC 9110 9.3.2: same handler and head fields,
// body suppressed); websocket GET routes require an explicit HEAD route.
inline constexpr std::array ruvia_all_route_methods = {http_known_method::get, http_known_method::post, http_known_method::put, http_known_method::patch, http_known_method::delete_, http_known_method::options};

// Startup-only holder for the RUVIA_ON method list; the macro pastes the
// parenthesized list as a constructor call.
template <std::size_t n>
class ruvia_method_list final {
public:
    template <std::same_as<http_known_method>... methods_type>
    constexpr explicit ruvia_method_list(methods_type... methods) noexcept :
        methods_{methods...} {
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
    constexpr explicit ruvia_path_list(paths_type&&... paths) noexcept :
        paths_{borrowed_text(std::forward<paths_type>(paths)).view()...} {
        static_assert(sizeof...(paths_type) > 0, "RUVIA_ON requires at least one path");
        static_assert(sizeof...(paths_type) == n);
    }

    template <typename... paths_type>
        requires((std::convertible_to<paths_type&&, std::string_view> && ...) && (!std::constructible_from<borrowed_text, paths_type&&> || ...))
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

#define RUVIA_CONTROLLER_GROUP(prefix, ...)                                                                                                                \
private:                                                                                                                                                   \
    [[nodiscard]] static constexpr ::std::string_view ruvia_controller_group_prefix() noexcept {                                                           \
        static_assert(::std::constructible_from<::ruvia::borrowed_text, decltype((prefix))>, "controller group prefixes must outlive route registration"); \
        return ::ruvia::borrowed_text(prefix).view();                                                                                                      \
    }                                                                                                                                                      \
    static constexpr auto ruvia_controller_group_models() noexcept {                                                                                       \
        return ::ruvia::detail::route_models<__VA_ARGS__>();                                                                                               \
    }                                                                                                                                                      \
    [[nodiscard]] static auto ruvia_controller_group_middlewares() {                                                                                       \
        return ::ruvia::detail::controller_registration_access<ruvia_controller_type>::template make_middlewares<__VA_ARGS__>();                           \
    }

// A controller is a final, default-constructible class that derives from
// nothing. RUVIA_ROUTES_BEGIN names it because the registration flag
// RUVIA_ROUTES_END declares is a static data member initializer, which is not a
// complete-class context and so cannot deduce the enclosing type.
#define RUVIA_ROUTES_BEGIN(controller_type)                                                                                                                                                      \
private:                                                                                                                                                                                         \
    using ruvia_controller_type = controller_type;                                                                                                                                               \
    using ruvia_controller_access_type = ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                                                                                 \
    friend class ::ruvia::detail::controller_registration_access<ruvia_controller_type>;                                                                                                         \
    void register_routes(::ruvia::detail::router& router) {                                                                                                                                      \
        static_assert(::std::is_same_v<ruvia_controller_type, ::std::remove_cvref_t<decltype(*this)>>, "RUVIA_ROUTES_BEGIN must name its enclosing controller");                                 \
        auto ruvia_controller_group = ruvia_controller_access_type::create_route_group(router, ruvia_controller_access_type::group_prefix(), ruvia_controller_access_type::group_middlewares()); \
        [[maybe_unused]] constexpr auto ruvia_scope_models = ruvia_controller_access_type::group_models();                                                                                       \
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

#define RUVIA_ROUTES_END \
    }                    \
    RUVIA_DETAIL_CONTROLLER_RETAIN inline static const bool ruvia_controller_registered_ = ::ruvia::detail::add_controller_registrar(::ruvia::detail::controller_registrar_of<ruvia_controller_type>());

#define RUVIA_DETAIL_BIND_ROUTE(handler, ...) ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this, ruvia_scope_models + ::ruvia::detail::route_models<__VA_ARGS__>())

#define RUVIA_DETAIL_ADD_METHOD_ROUTE(method, path, handler, ...) ruvia_controller_access_type::add_route(ruvia_route_scope, method, {}, path, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Route paths and request paths compare in one canonical form
// (ruvia::uri_paths_equivalent): segments split on literal '/', each compared
// by its percent-decoded octets, so "/users/%61dmin" reaches the "/users/admin"
// route and its middleware, hex case never matters, and "%2F" stays inside its
// segment. Path-scoped middleware, scoped fallbacks and decoded route
// parameters use the same form; two patterns differing only in spelling are a
// duplicate registration. A GET route also answers HEAD unless it is a
// websocket route (see ruvia_all_route_methods).
#define RUVIA_GET(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::get, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_SELECT_SECOND(first, second, ...) second
#define RUVIA_DETAIL_CHOOSE_VA_MACRO(with_args, without_args, ...) RUVIA_DETAIL_SELECT_SECOND(__VA_OPT__(, ) with_args, without_args)

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS(protocol, target, handler) ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target, RUVIA_DETAIL_BIND_ROUTE(handler), ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS(protocol, target, handler, first, ...)                                                                                                                                                                                                          \
    [&](auto ruvia_route_probe) {                                                                                                                                                                                                                                                               \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0, ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                                                                                                                                  \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::http_tunnel_route_config>) {                                                                                                                                                                                              \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(), static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));         \
        } else {                                                                                                                                                                                                                                                                                \
            ruvia_controller_access_type::add_tunnel_route(ruvia_route_scope, protocol, target, RUVIA_DETAIL_BIND_ROUTE(handler, ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>()); \
        }                                                                                                                                                                                                                                                                                       \
    }(::ruvia::detail::route_arg_probe{})

// Ordinary CONNECT uses an exact host:port authority or "*". The only
// upgrade protocol is "connect-udp"; any other fails registration with
// std::invalid_argument. Group prefixes apply only to upgrade-protocol tunnel
// paths; controller/group middleware always runs.
#define RUVIA_CONNECT(authority, handler, ...) RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)({}, authority, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_CONNECT_PROTOCOL(protocol, path, handler, ...) RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_TUNNEL_ROUTE_WITH_ARGS, RUVIA_DETAIL_ADD_TUNNEL_ROUTE_NO_ARGS, __VA_ARGS__)(protocol, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS(path, handler) ruvia_controller_access_type::add_websocket_route(ruvia_route_scope, ::ruvia::http_known_method::get, path, RUVIA_DETAIL_BIND_ROUTE(handler), ruvia_controller_access_type::template make_middlewares<>())

#define RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS(path, handler, first, ...)                                                                                                                                                                                                                                                  \
    [&](auto ruvia_route_probe) {                                                                                                                                                                                                                                                                                       \
        using ruvia_first_arg_type = ::std::conditional_t<sizeof(ruvia_route_probe) != 0, ::std::remove_cvref_t<decltype((first) + ruvia_route_probe)>, void>;                                                                                                                                                          \
        if constexpr (::std::is_same_v<ruvia_first_arg_type, ::ruvia::websocket_route_config>) {                                                                                                                                                                                                                        \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope, ::ruvia::http_known_method::get, path, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>(), static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe));         \
        } else {                                                                                                                                                                                                                                                                                                        \
            ruvia_controller_access_type::add_websocket_route(ruvia_route_scope, ::ruvia::http_known_method::get, path, RUVIA_DETAIL_BIND_ROUTE(handler, ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<ruvia_first_arg_type __VA_OPT__(, ) __VA_ARGS__>()); \
        }                                                                                                                                                                                                                                                                                                               \
    }(::ruvia::detail::route_arg_probe{})

#define RUVIA_GET_WS(path, handler, ...) RUVIA_DETAIL_CHOOSE_VA_MACRO(RUVIA_DETAIL_ADD_WS_ROUTE_WITH_ARGS, RUVIA_DETAIL_ADD_WS_ROUTE_NO_ARGS, __VA_ARGS__)(path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_POST(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::post, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PUT(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::put, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_DELETE(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::delete_, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_PATCH(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::patch, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_HEAD(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::head, path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_OPTIONS(path, handler, ...) RUVIA_DETAIL_ADD_METHOD_ROUTE(::ruvia::http_known_method::options, path, handler __VA_OPT__(, ) __VA_ARGS__)

// An extension method, routed by its exact wire token: PROPFIND, PURGE, LOCK.
// HTTP's method space is open (http_known_method is only this framework's fixed
// classification, never the wire value), but routes were keyed on that enum, so
// anything outside it answered 501 and could not be served at all.
//
// The token is case-sensitive per RFC 9110 9.1, must be a valid method token,
// and must NOT be one of the classified methods -- those keep the enum as their
// identity and their own macros. The path must be static; only the classified
// methods have dynamic-segment routes.
#define RUVIA_METHOD(method_token, path, handler, ...) ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::unknown, method_token, path, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Hono app.all: registers the handler for GET/POST/PUT/PATCH/DELETE/OPTIONS.
#define RUVIA_ALL(path, handler, ...)                                                                                                                                                                                                    \
    {                                                                                                                                                                                                                                    \
        auto&& ruvia_route_path = (path);                                                                                                                                                                                                \
        for (const auto ruvia_route_method : ::ruvia::detail::ruvia_all_route_methods) {                                                                                                                                                 \
            ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {}, ruvia_route_path, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>()); \
        }                                                                                                                                                                                                                                \
    }

// Hono app.on: registers the handler for an explicit method x path list, e.g.
// RUVIA_ON((ruvia::http_known_method::put, ruvia::http_known_method::delete_),
//     ("/items/:id", "/legacy/:id"), handler).
#define RUVIA_ON(methods, paths, handler, ...)                                                                                                                                                                                               \
    {                                                                                                                                                                                                                                        \
        auto ruvia_route_methods = ::ruvia::detail::ruvia_method_list methods;                                                                                                                                                               \
        auto ruvia_route_paths = ::ruvia::detail::ruvia_path_list paths;                                                                                                                                                                     \
        for (const auto ruvia_route_method : ruvia_route_methods) {                                                                                                                                                                          \
            for (const auto ruvia_route_path : ruvia_route_paths) {                                                                                                                                                                          \
                ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method, {}, ruvia_route_path, RUVIA_DETAIL_BIND_ROUTE(handler, __VA_ARGS__), ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>()); \
            }                                                                                                                                                                                                                                \
        }                                                                                                                                                                                                                                    \
    }
#define RUVIA_GROUP_BEGIN(prefix, ...) \
    ruvia_controller_access_type::template with_route_group<__VA_ARGS__>(ruvia_route_scope, prefix, ruvia_scope_models, [&](const auto& ruvia_route_scope, [[maybe_unused]] auto ruvia_scope_models) {
#define RUVIA_GROUP_END \
    });
