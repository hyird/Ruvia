#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/web_worker.h"
#include "ruvia/web/websocket.h"

namespace ruvia {

class request_stream;
class http_client;
#ifdef RUVIA_ENABLE_DATABASE
class db_client;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_client;
#endif
namespace detail {
class router_state;
}

using router_handler = move_only_function<task<http_response>(context&)>;
using router_stream_handler = move_only_function<task<void>(context&)>;

// An explicitly owned, worker-local route engine, independent of application
// and controller discovery. Configure and freeze on its owner worker, then
// await dispatch(stream) from a request_stream callback on that same worker.
// Handlers and middleware instances belong to this router; concurrent requests
// interleave only on its worker. Use one router per worker, not shared mutable
// handlers across workers. Captured references must outlive all dispatches.
// The worker is retained. Configuration, freezing and destruction of the last
// owner run on that worker. A dispatch task retains the frozen state, so the
// facade need not outlive that task; the request still must.
class router final {
public:
    explicit router(worker_ref worker);
    ~router();
    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    // Methods are case-sensitive HTTP tokens. Extension methods have the same
    // static-path restriction as controller routes. Paths and callbacks are
    // owned before return; modifiers reuse controller middleware/body policies.
    template <route_modifier... modifiers>
    router& add(std::string_view method, std::string_view path, router_handler handler) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_buffered(method, path, std::move(handler), descriptors);
    }

    template <route_modifier... modifiers>
    router& stream(std::string_view method, std::string_view path, router_stream_handler handler) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_stream(method, path, std::move(handler), descriptors);
    }

    // The WebSocket route of `path`: an HTTP/1.1 Upgrade GET or an extended
    // CONNECT (RFC 8441, RFC 9220). The terminal after the route's middleware
    // negotiates through request_stream::accept_websocket(); the handler then
    // runs on context::websocket(). A rejected handshake answers 400 without
    // the handler, as does a request to the path offering no upgrade.
    template <route_modifier... modifiers>
    router& websocket(std::string_view path, router_stream_handler handler, websocket_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_websocket(path, std::move(handler), std::move(config), descriptors);
    }

    // CONNECT to an exact host:port authority, or "*" for any (RFC 9110
    // §9.3.6). The terminal establishes the tunnel with 200 through
    // request_stream::accept_tunnel(); the handler then runs on
    // context::tunnel().
    template <route_modifier... modifiers>
    router& connect(std::string_view authority, router_stream_handler handler, http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel({}, authority, std::move(handler), config, descriptors);
    }

    // An extended CONNECT protocol on `path`; "connect-udp" (RFC 9298) also
    // accepts its HTTP/1.1 Upgrade (with 101) and implies HTTP Datagrams.
    template <route_modifier... modifiers>
    router& connect_protocol(std::string_view protocol, std::string_view path, router_stream_handler handler,
        http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel(protocol, path, std::move(handler), config, descriptors);
    }

    template <middleware middleware_type, typename... args_types>
    router& use(args_types&&... args) & {
        return add_middleware(detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    template <middleware middleware_type, typename... args_types>
    router& use_at(middleware_scope_options options, args_types&&... args) & {
        static_assert(!detail::middleware_runs_on_unmatched_requests<middleware_type>(),
            "middleware serving unmatched requests must be registered with use<T>()");
        return add_scoped_middleware(options.prefix_.view(),
            detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    // The rule every request this router dispatches passes, matched or not,
    // keyed on the client address like application::rate_limit(). Route
    // ruvia::rate_limit<> rules stack on it. Counted per router, which is
    // per worker; replaces an earlier rule.
    router& rate_limit(rate_limit_config config) &;

    // Lend caller-owned clients and per-worker state to the requests this
    // router dispatches, so context::redis(alias), db(alias), client(alias)
    // and worker_state<T>() -- and middleware built on them, such as
    // session_middleware -- resolve here. A router with any bound client
    // resolves every client lookup only among its own, and one with any bound
    // state every state lookup; otherwise the host's registrations apply.
    // Each client must run on this router's worker (std::invalid_argument
    // otherwise) and, like each state, outlive every dispatch: shut it down
    // only after the host serving this router has shut down. Aliases are
    // unique per kind; "default" is what alias-less lookups name.
#ifdef RUVIA_ENABLE_REDIS
    router& bind_redis(std::string_view alias, const redis_client& client) &;
    router& bind_redis(std::string_view, const redis_client&&) & = delete;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    router& bind_database(std::string_view alias, const db_client& client) &;
    router& bind_database(std::string_view, const db_client&&) & = delete;
#endif
    // alias names this client's default configuration in this router.
    router& bind_http_client(std::string_view alias, const http_client& client) &;
    router& bind_http_client(std::string_view, const http_client&&) & = delete;
    // One instance per type; the router never destroys it.
    template <typename state_type>
    router& bind_worker_state(state_type& state) & {
        return add_worker_state(detail::worker_state_definition::borrow(state));
    }

    router& on_error(http_error_handler handler) &;
    router& on_not_found(http_not_found_handler handler) &;
    router& on_error(scoped_error_handler_options options) &;
    router& on_not_found(scoped_not_found_handler_options options) &;

    // Compiles the shared route/method/parameter/middleware engine and creates
    // this worker's middleware once. Failure leaves configuration editable;
    // success seals every registration. Repeated freeze is a logic error.
    void freeze() &;
    void freeze() && = delete;

    // Single-shot for each request, before another response has been selected.
    // Uses the transport's cancellation, body budgets, response writer,
    // WebSocket/tunnel acceptance and connection services. HEAD/OPTIONS/405/
    // 501, errors, middleware, URL generation and buffered subrequests use the
    // normal route engine.
    // Cold operations retain router state and belong to the request's scope;
    // neither a saved operation nor a handler may outlive its request.
    [[nodiscard]] task<void> dispatch(request_stream& stream) &;
    task<void> dispatch(request_stream&) && = delete;

private:
    router& add_buffered(std::string_view method, std::string_view path, router_handler handler,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_stream(std::string_view method, std::string_view path, router_stream_handler handler,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config,
        std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler,
        http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_worker_state(detail::worker_state_definition definition);
    router& add_middleware(detail::owned_middleware_descriptor descriptor);
    router& add_scoped_middleware(std::string_view prefix, detail::owned_middleware_descriptor descriptor);

    std::shared_ptr<detail::router_state> state_;
};

}  // namespace ruvia

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
[[nodiscard]] inline std::string_view normalize_fallback_prefix(std::string_view prefix) {
    if (prefix.empty() || prefix.front() != '/') {
        throw std::invalid_argument("fallback prefix must start with '/'");
    }
    if ((prefix.find('?') != std::string_view::npos) || !is_valid_http_origin_form_target(prefix)) {
        throw std::invalid_argument("fallback prefix must be an origin-form path without query");
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    if (prefix == "/") {
        throw std::invalid_argument("fallback prefix must not be '/'; register app-wide without a prefix");
    }
    return prefix;
}

// The one prefix-scoping rule, shared by fallback handler selection and by
// path-scoped middleware. `prefix` is normalized (see normalize_fallback_prefix)
// or empty for an unscoped registration, which scopes everything.
// A prefix matches on whole path segments only, so "/api" scopes "/api" and
// "/api/x" but never "/apix". Segments compare in the
// canonical form route matching and route parameters use (uri_paths_equivalent):
// split on raw '/' and percent-decoded, so "/%61pi/x" is under "/api" exactly
// when it routes like "/api/x", while an encoded "%2F" never acts as a segment
// boundary.
[[nodiscard]] inline bool path_is_under_prefix(
    std::string_view path, std::string_view prefix) noexcept {
    if (prefix.empty()) {
        return true;
    }
    if (path.empty() || path.front() != '/') {
        return false;
    }
    std::size_t path_cursor = 1;
    std::size_t prefix_cursor = 1;
    for (;;) {
        const auto prefix_slash = prefix.find('/', prefix_cursor);
        const auto prefix_end = prefix_slash == std::string_view::npos ? prefix.size() : prefix_slash;
        const auto path_slash = path.find('/', path_cursor);
        const auto path_end = path_slash == std::string_view::npos ? path.size() : path_slash;
        if (!uri_paths_equivalent(path.substr(path_cursor, path_end - path_cursor),
                prefix.substr(prefix_cursor, prefix_end - prefix_cursor))) {
            return false;
        }
        if (prefix_end == prefix.size()) {
            // path_end is a '/' or the end of the path: a segment boundary.
            return true;
        }
        if (path_end == path.size()) {
            return false;
        }
        prefix_cursor = prefix_end + 1;
        path_cursor = path_end + 1;
    }
}

// Two normalized prefixes that scope the same request paths, such as "/api"
// and "/%61pi", are one registration.
[[nodiscard]] inline bool fallback_prefixes_equivalent(std::string_view left, std::string_view right) noexcept {
    return path_is_under_prefix(left, right) && path_is_under_prefix(right, left);
}

// Segment count of a normalized prefix. A scope nested inside another always
// has more segments, whatever either one's escaping.
[[nodiscard]] inline std::size_t fallback_prefix_depth(std::string_view prefix) noexcept {
    std::size_t depth = 0;
    for (const auto c : prefix) {
        depth += c == '/' ? 1U : 0U;
    }
    return depth;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

class router;

template <typename callback_type>
using callback_registration = std::unique_ptr<callback_type>;

// Pin configured callables before entering the application lock: publication
// only swaps owning pointers, and router views borrow an address-stable owner.
template <typename signature_type>
[[nodiscard]] callback_registration<move_only_function<signature_type>> own_callback(
    move_only_function<signature_type>&& callback) {
    if (!callback) {
        return {};
    }
    return std::make_unique<move_only_function<signature_type>>(std::move(callback));
}

template <middleware middleware_type, typename... args_types>
[[nodiscard]] owned_middleware_descriptor make_scoped_app_middleware(
    const middleware_scope_options& options, args_types&&... args) {
    static_assert(!middleware_runs_on_unmatched_requests<middleware_type>(),
        "a middleware declaring ruvia_runs_on_unmatched_requests cannot be path-scoped with "
        "use_at(); register it app-wide with use<T>()");
    const auto normalized = normalize_fallback_prefix(options.prefix_.view());
    return make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...)
        .scoped_to(normalized);
}

template <typename state_type>
[[nodiscard]] worker_state_definition make_default_worker_state() {
    static_assert(std::is_default_constructible_v<state_type>,
        "use_worker_state<T>() without a factory requires T to be default "
        "constructible; pass a factory otherwise");
    return worker_state_definition::make<state_type>([] { return state_type(); });
}

// application and test_app own this same dispatch configuration. Their lifecycle guards
// remain outside the component; validation, callback ownership, registration
// storage and router binding have one implementation. Bound callback views
// borrow this immovable owner until its routers have retired.
class app_configuration final {
public:
    app_configuration();

    app_configuration(const app_configuration&) = delete;
    app_configuration& operator=(const app_configuration&) = delete;
    app_configuration(app_configuration&&) = delete;
    app_configuration& operator=(app_configuration&&) = delete;

    void add_middleware(owned_middleware_descriptor&& descriptor);
    void add_worker_state(worker_state_definition&& definition);
    void on_error(callback_registration<http_error_handler>&& handler);
    void on_error(borrowed_text prefix, callback_registration<http_error_handler>&& handler);
    // Returns the cleared handler so the caller destroys it after releasing its lock.
    [[nodiscard]] callback_registration<http_error_handler> on_error(std::nullptr_t) noexcept;
    void on_not_found(callback_registration<http_not_found_handler>&& handler);
    void on_not_found(borrowed_text prefix, callback_registration<http_not_found_handler>&& handler);
    [[nodiscard]] callback_registration<http_not_found_handler> on_not_found(std::nullptr_t) noexcept;

    [[nodiscard]] std::span<const worker_state_definition> worker_states() const noexcept {
        return worker_states_;
    }

    void apply(router& routes) const;

private:
    callback_registration<http_error_handler> error_handler_;
    callback_registration<http_not_found_handler> not_found_handler_;
    std::vector<std::pair<std::string, callback_registration<http_error_handler>>> prefix_error_handlers_;
    std::vector<std::pair<std::string, callback_registration<http_not_found_handler>>> prefix_not_found_handlers_;
    std::vector<owned_middleware_descriptor> middlewares_;
    std::vector<worker_state_definition> worker_states_;
};

}  // namespace ruvia::detail
