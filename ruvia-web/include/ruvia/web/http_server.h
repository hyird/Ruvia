#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/web/connection_events.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/server_connection.h"
#include "ruvia/web/server_observation.h"

namespace ruvia {

namespace detail {
class http_server_state;
class http_server_group_state;
}  // namespace detail

struct http_server_callbacks final {
    request_stream_callback request_stream_;
    connection_callback connection_;
    access_log_callback access_;
    connection_failure_callback connection_failure_;
    tls_client_hello_callback tls_client_hello_;
    http2_input_callback http2_input_;
#if defined(RUVIA_ENABLE_HTTP2)
    http2_frame_callback http2_frame_;
    http2_headers_callback http2_headers_;
#endif
    connection_admission_callback connection_admission_;
    connection_open_callback connection_open_;
    connection_close_callback connection_close_;
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

    // Before start only. Replaces the complete trusted set atomically; invalid
    // CIDRs leave it unchanged. Empty means trust no forwarded headers.
    http_server& trusted_proxies(trusted_proxy_config config) &;
    http_server& trusted_proxies(trusted_proxy_config) && = delete;
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

    // Owner loop, while running. Requests whole-connection termination, not
    // merely a stream reset. False means no live connection has this ID.
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) &;
    bool close_connection(std::uint64_t) && = delete;

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

// A service shared by caller-owned loops. Callbacks may run concurrently.
// Configuration before start does not require the loops to be driven.
// start(), shutdown() and running controls block, must run outside every
// member loop, and require the caller to drive all live loops until completion.
// A valid but undriven loop cannot be detected. close() is any-thread and never
// stops a caller loop; a member loop's retirement closes the entire group.
class http_server_group final {
public:
    http_server_group(std::span<const event_loop> loops, http_server_callbacks callbacks, server_config config = {});
    ~http_server_group();
    http_server_group(const http_server_group&) = delete;
    http_server_group& operator=(const http_server_group&) = delete;
    http_server_group(http_server_group&&) = delete;
    http_server_group& operator=(http_server_group&&) = delete;

    void listen(listen_config config) &;
    void add_connection_service(listen_config config) &;
    // Before start only; replaces the complete set on every worker. Parsing
    // and allocation finish before publication; invalid input changes nothing.
    http_server_group& trusted_proxies(trusted_proxy_config config) &;
    // Prepare every worker before opening any listener; failure rolls back
    // the whole host without cancelling unrelated caller work.
    void start() &;
    void remove_listener(const listener_endpoint& endpoint) &;
    void replace_tls_identities(const listener_endpoint& endpoint, tls_config config) &;
#ifndef _WIN32
    [[nodiscard]] listener_sockets duplicate_listener_sockets(const listener_endpoint& endpoint) const&;
    listener_sockets duplicate_listener_sockets(const listener_endpoint&) const&& = delete;
#endif
    // Explicit target worker; takes socket ownership even when rejected.
    void adopt(std::size_t worker_index, const listener_endpoint& service, native_socket socket) &;
    // Blocking external control, as remove_listener(). IDs never name a
    // different connection after retirement; false means the ID is not live.
    [[nodiscard]] bool close_connection(std::uint64_t connection_id) &;
    bool close_connection(std::uint64_t) && = delete;
    void close() noexcept;
    void shutdown() &;
    [[nodiscard]] http_server_stats stats() const noexcept;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] const worker_handle& worker(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    const worker_handle& worker(std::size_t) const&& = delete;

private:
    std::shared_ptr<detail::http_server_group_state> state_;
};

}  // namespace ruvia
