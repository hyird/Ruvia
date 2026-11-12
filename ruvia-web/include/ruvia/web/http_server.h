#pragma once

#include <memory>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/server_connection.h"
#include "ruvia/web/server_observation.h"

namespace ruvia {

namespace detail {
class http_server_state;
}

struct http_server_callbacks final {
    request_stream_callback request_stream_;
    connection_callback connection_;
    access_log_callback access_;
    connection_failure_callback connection_failure_;
    tls_client_hello_callback tls_client_hello_;
    http2_input_callback http2_input_;
};

// One inbound service host on a caller-owned event loop. No application,
// controller registry, business thread or process signal handler is involved.
// Windows listeners reuse the dedicated native acceptor. Runtime scheduling
// fields in server_config belong to the caller's loop; limits apply here to
// this one worker. The host owns callbacks until every session has retired.
class http_server final {
public:
    http_server(event_loop loop, http_server_callbacks callbacks, server_config config = {});
    ~http_server();
    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;
    http_server(http_server&&) = delete;
    http_server& operator=(http_server&&) = delete;

    // Owner loop only. Before start(), stages a listener; afterwards adds it
    // atomically. Uses the same validation, credential loading and inherited
    // socket ownership as application::listen(). dispatch_ must be
    // request_stream or connection: this host does not register routes.
    void listen(listen_config config) &;
    void listen(listen_config) && = delete;
    // Stages or adds a TCP/TLS service without binding a listening socket.
    // http_/https_ identify it for adopt(); HTTP/3, inherited listeners and
    // automatic redirects require listen() instead.
    void add_connection_service(listen_config config) &;
    void add_connection_service(listen_config) && = delete;
    // Takes ownership even on failure. The socket must be connected and not
    // associated with another Windows IOCP; it is first bound to this loop.
    // Uses the same TLS handshake, budgets, timeouts and session drivers as
    // a connection this host accepted itself. Owner loop, after start().
    void adopt(const listener_endpoint& service, native_socket socket) &;
    void adopt(const listener_endpoint&, native_socket) && = delete;

    // Owner loop only. Starts every staged service once; a failure closes and
    // joins all prepared resources before propagating. An empty host may start
    // and have services added later.
    [[nodiscard]] task<void> start() &;
    task<void> start() && = delete;
    // Owner loop only. Stops new admission immediately; established sessions
    // continue until they end or close(). The endpoint may then be added again.
    void remove_listener(const listener_endpoint& endpoint) &;
    void remove_listener(const listener_endpoint&) && = delete;
    void replace_tls_identities(const listener_endpoint& endpoint, tls_config config) &;
    void replace_tls_identities(const listener_endpoint&, tls_config) && = delete;
#ifndef _WIN32
    [[nodiscard]] listener_sockets duplicate_listener_sockets(const listener_endpoint& endpoint) const&;
    listener_sockets duplicate_listener_sockets(const listener_endpoint&) const&& = delete;
#endif

    // Any thread; cancels only this host, not unrelated work on the loop.
    // Destruction requests the same close. The loop must keep running until
    // shutdown completes, or its own retirement has joined the stop callback.
    void close() noexcept;
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] const worker_handle& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_server_state> state_;
};

}  // namespace ruvia
