#pragma once

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "callback_ref.h"
#include "controller.h"
#include "http_error.h"
#include "http_tunnel.h"
#include "middleware.h"
#include "move_only_function.h"
#include "rate_limiting.h"
#include "task.h"
#include "websocket.h"
#include "worker_ref.h"
#include "worker_state.h"

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
class router final : public dispatch_configuration<router> {
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
    router& add(std::string_view, std::string_view, router_handler) && = delete;

    template <route_modifier... modifiers>
    router& stream(std::string_view method, std::string_view path, router_stream_handler handler) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_stream(method, path, std::move(handler), descriptors);
    }
    template <route_modifier... modifiers>
    router& stream(std::string_view, std::string_view, router_stream_handler) && = delete;

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
    template <route_modifier... modifiers>
    router& websocket(std::string_view, router_stream_handler, websocket_route_config = {}) && = delete;

    // CONNECT to an exact host:port authority, or "*" for any (RFC 9110
    // §9.3.6). The terminal establishes the tunnel with 200 through
    // request_stream::accept_tunnel(); the handler then runs on
    // context::tunnel().
    template <route_modifier... modifiers>
    router& connect(std::string_view authority, router_stream_handler handler, http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel({}, authority, std::move(handler), config, descriptors);
    }
    template <route_modifier... modifiers>
    router& connect(std::string_view, router_stream_handler, http_tunnel_route_config = {}) && = delete;

    // An extended CONNECT protocol on `path`; "connect-udp" (RFC 9298) also
    // accepts its HTTP/1.1 Upgrade (with 101) and implies HTTP Datagrams.
    template <route_modifier... modifiers>
    router& connect_protocol(std::string_view protocol, std::string_view path, router_stream_handler handler, http_tunnel_route_config config = {}) & {
        const std::array<detail::controller_middleware_descriptor, sizeof...(modifiers)> descriptors{detail::make_route_modifier_descriptor<modifiers>()...};
        return add_tunnel(protocol, path, std::move(handler), config, descriptors);
    }
    template <route_modifier... modifiers>
    router& connect_protocol(std::string_view, std::string_view, router_stream_handler, http_tunnel_route_config = {}) && = delete;

    // use(), use_at(), use_worker_state(), on_error(), on_not_found() and
    // rate_limit() are the dispatch_configuration members application shares.
    // Here they apply to the requests this router dispatches, are counted and
    // instantiated per router (which is per worker), and are accepted on the
    // owner worker until freeze().

    // Lend caller-owned clients and per-worker state to the requests this
    // router dispatches, so worker_services_access::redis(alias), db(alias), client(alias)
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
    router& bind_redis(std::string_view, const redis_client&) && = delete;
    router& bind_redis(std::string_view, const redis_client&&) = delete;
#endif
#ifdef RUVIA_ENABLE_DATABASE
    router& bind_db(std::string_view alias, const db_client& client) &;
    router& bind_db(std::string_view, const db_client&) && = delete;
    router& bind_db(std::string_view, const db_client&&) = delete;
#endif
    // alias names this client's default configuration in this router.
    router& bind_http_client(std::string_view alias, const http_client& client) &;
    router& bind_http_client(std::string_view, const http_client&) && = delete;
    router& bind_http_client(std::string_view, const http_client&&) = delete;
    // One caller-owned instance per type; the router never destroys it. The
    // lending counterpart of use_worker_state(), whose instance it owns.
    template <typename state_type>
    router& bind_worker_state(state_type& state) & {
        return add_worker_state(detail::worker_state_definition::borrow(state));
    }
    template <typename state_type>
    router& bind_worker_state(state_type&) && = delete;

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
    friend class dispatch_configuration<router>;

    router& add_buffered(std::string_view method, std::string_view path, router_handler handler, std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_stream(std::string_view method, std::string_view path, router_stream_handler handler, std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_websocket(std::string_view path, router_stream_handler handler, websocket_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers);
    router& add_tunnel(std::string_view protocol, std::string_view target, router_stream_handler handler, http_tunnel_route_config config, std::span<const detail::controller_middleware_descriptor> modifiers);
    void edit_configuration(detail::callback_ref<void(detail::app_configuration&)> edit);

    std::shared_ptr<detail::router_state> state_;
};

}  // namespace ruvia
