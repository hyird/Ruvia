#pragma once

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/core/move_only_function.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/rate_limit.h"
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
