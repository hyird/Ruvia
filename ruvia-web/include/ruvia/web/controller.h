#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/web/detail/controller/controller_runtime.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/validation.h"

namespace ruvia {

class context;

// A request-body ceiling for the routes it is registered on:
//
//     RUVIA_POST("/avatar", upload, ruvia::body_limit<256 * 1024>);
//
// server_config::max_buffered_body_bytes_ has to be sized for the largest body any
// route accepts,
// which leaves every other route accepting that much too. This is the same
// policy at a narrower scope: an endpoint expecting a small JSON document stops
// reading at a small JSON document.
//
// One rule governs every policy that has both an app-wide and a route-level
// form: the narrower scope may only TIGHTEN. A route cannot lift the
// deployment-wide bound, and where a controller-wide and a route-specific
// declaration both exist the stricter wins rather than the nearer. On a stream
// route this also gives an otherwise unbounded body a bound.
//
// This is a declaration the server reads BEFORE the body is accepted, not code
// that runs in the chain -- by the time a middleware's handle() runs, the bytes
// it would have rejected are already buffered. Registering it on a controller
// applies it to that controller's routes, and where both a controller-wide and
// a route-specific one exist the stricter wins. use() and use_at() reject it
// with std::invalid_argument; the app-wide bound is max_buffered_body_bytes_.
template <std::size_t max_bytes>
class body_limit final {
public:
    static_assert(max_bytes > 0, "body limit must be greater than 0");

    static constexpr std::size_t ruvia_request_body_limit_bytes = max_bytes;

    task<void> handle(context&, next& next_value) {
        co_await next_value();
    }
};

}  // namespace ruvia

namespace ruvia::detail {

// Methods covered by RUVIA_ALL. HEAD is intentionally omitted: buffered GET
// routes provide an implicit HEAD fallback without another route record, while
// streaming/SSE/websocket routes require an explicit HEAD route.
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

#define RUVIA_GET(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::get, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),           \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

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
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe),                            \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());                   \
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
                static_cast<ruvia_first_arg_type>((first) + ruvia_route_probe),                            \
                ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>());                   \
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

#define RUVIA_WS(path, handler, ...) RUVIA_GET_WS(path, handler __VA_OPT__(, ) __VA_ARGS__)

#define RUVIA_POST(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::post, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),            \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

#define RUVIA_PUT(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::put, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),           \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

#define RUVIA_DELETE(path, handler, ...)                                                                       \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::delete_value, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),                    \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

#define RUVIA_PATCH(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::patch, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),             \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

#define RUVIA_HEAD(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::head, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),            \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

#define RUVIA_OPTIONS(path, handler, ...)                                                                 \
    ruvia_controller_access_type::add_route(ruvia_route_scope, ::ruvia::http_known_method::options, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),               \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// An extension method, routed by its exact wire token: PROPFIND, PURGE, LOCK.
// HTTP's method space is open (http_known_method is only this framework's fixed
// classification, never the wire value), but routes were keyed on that enum, so
// anything outside it answered 501 and could not be served at all.
//
// The token is case-sensitive per RFC 9110 9.1, must be a valid method token,
// and must NOT be one of the classified methods -- those keep the enum as their
// identity and their own macros. The path must be static: extension routes are
// matched by a cold linear scan with no dynamic-segment index behind it.
#define RUVIA_METHOD(method_token, path, handler, ...)                                              \
    ruvia_controller_access_type::add_extension_method_route(ruvia_route_scope, method_token, path, \
        ruvia_controller_access_type::template bind<&ruvia_controller_type::handler>(this),         \
        ruvia_controller_access_type::template make_middlewares<__VA_ARGS__>())

// Hono app.all: registers the handler for GET/POST/PUT/PATCH/DELETE/OPTIONS.
#define RUVIA_ALL(path, handler, ...)                                                               \
    {                                                                                               \
        auto&& ruvia_route_path = (path);                                                           \
        for (const auto ruvia_route_method : ::ruvia::detail::ruvia_all_route_methods) {            \
            ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method,          \
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
                ruvia_controller_access_type::add_route(ruvia_route_scope, ruvia_route_method,          \
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
