#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/move_only_function.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/web/connection_events.h"
#include "ruvia/web/prepared_listener.h"
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
#if defined(RUVIA_ENABLE_HTTP3)
    http3_frame_callback http3_frame_;
    http3_field_callback http3_field_;
#endif
    connection_admission_callback connection_admission_;
    connection_open_callback connection_open_;
    connection_close_callback connection_close_;
};

// One inbound service host on a caller-owned event loop. No application,
// controller registry, business thread or process signal handler is involved.
// Windows listeners reuse the dedicated native acceptor. Runtime scheduling
// fields in server_config belong to the caller's loop; limits apply here to
// this one worker. The host owns callbacks until every session has retired;
// they are released on the loop once shutdown has joined every session.
class http_server final {
public:
    http_server(event_loop loop, http_server_callbacks callbacks, server_config config = {});
    ~http_server();
    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;
    http_server(http_server&&) = delete;
    http_server& operator=(http_server&&) = delete;

    // Owner loop, before start only: stages owned configuration without
    // loading credentials; start() loads and checks it, dispatch_ included.
    // Runtime additions use add_listener().
    http_server& listen(listen_config config) &;
    http_server& listen(listen_config) && = delete;
    [[nodiscard]] task<prepared_listener> prepare_listener(listen_config config) &;
    task<prepared_listener> prepare_listener(listen_config) && = delete;
    // 整批准备并共享一次提交/回滚；所有配置均就绪后才返回。
    // 地址/TLS 配置在调用时复制，继承 socket 的所有权同时转入，包括冷任务。
    [[nodiscard]] task<prepared_listener> prepare_listeners(std::span<const listen_config> configs) &;
    task<prepared_listener> prepare_listeners(std::span<const listen_config>) && = delete;
    [[nodiscard]] task<void> add_listener(listen_config config) &;
    task<void> add_listener(listen_config) && = delete;
    // Stages or adds a TCP/TLS service without binding a listening socket.
    // http_/https_ identify it for adopt(); HTTP/3, inherited listeners and
    // automatic redirects require listen() instead.
    [[nodiscard]] task<void> add_connection_service(listen_config config) &;
    task<void> add_connection_service(listen_config) && = delete;
    // Takes ownership even on failure. The socket must be connected and not
    // associated with another Windows IOCP; it is first bound to this loop.
    // Serves any running TCP service `service` names -- a connection service
    // or a listener -- with the same admission, PROXY header, TLS handshake,
    // budgets, timeouts and session drivers as a connection it accepted
    // itself. Owner loop, after start().
    void adopt(const listener_endpoint& service, native_socket socket, adopt_options options = {}) &;
    void adopt(const listener_endpoint&, native_socket, adopt_options = {}) && = delete;

    // Before start only. Replaces the complete trusted set atomically; invalid
    // CIDRs leave it unchanged. An empty config trusts no forwarded headers.
    http_server& trusted_proxies(trusted_proxy_config config) &;
    http_server& trusted_proxies(trusted_proxy_config) && = delete;
    // Owner loop only. Starts every staged service once; a failure closes and
    // joins all prepared resources before propagating. An empty host may start
    // and have services added later.
    [[nodiscard]] task<void> start() &;
    task<void> start() && = delete;
    // Owner loop only. Stops new admission immediately; by default established
    // sessions continue. close_connections_ closes their transports immediately;
    // session retirement is joined independently (also safe from their handlers).
    // The endpoint may then be added again.
    void remove_listener(const listener_endpoint& endpoint, listener_remove_options options = {}) &;
    void remove_listener(const listener_endpoint&, listener_remove_options = {}) && = delete;
    // Owner loop, while running. Completes once the listener `endpoint` names
    // -- serving, or the latest removed one still holding connections -- has
    // no live connection (listener_stats::active_connections_ reached zero),
    // at once when it has none. Composes "remove_listener, then wait, then
    // shutdown"; shutdown itself still closes connections directly. A
    // timeout or stop in `options` throws std::system_error (timed_out or
    // operation_canceled) and leaves the connections alone; so does the
    // host's shutdown (operation_canceled). An unknown endpoint throws
    // std::invalid_argument.
    [[nodiscard]] task<void> wait_connections_closed(const listener_endpoint& endpoint, operation_options options = {}) &;
    task<void> wait_connections_closed(const listener_endpoint&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> replace_tls_identities(const listener_endpoint& endpoint, tls_config config) &;
    task<void> replace_tls_identities(const listener_endpoint&, tls_config) && = delete;
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
    void close() & noexcept;
    void close() && = delete;
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;
    // Any thread.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_server_state> state_;
};

// Builds one worker's callbacks; see http_server_group.
using http_server_callbacks_factory = move_only_function<http_server_callbacks(const worker_ref&)>;
// A service shared by caller-owned loops. Configuration before start does not
// require the loops to be driven. Controls suspend without blocking member
// loops and resume on the explicit caller worker (which may itself be a
// member). Drive live loops until shutdown completes. close() never stops
// caller loops; member retirement closes the group.
//
// Each member serves its connections with its own callbacks, which the
// factory builds -- for example a per-worker router that the request_stream_
// callback dispatches to. The first start() or add_connection_service() builds
// callbacks once per member, in member order, on each member's own loop
// thread before preparing any listener (never concurrently). It destroys the
// factory before that first operation completes or fails. Every member must
// set the same callback members (presence, not targets), or the operation
// throws std::invalid_argument. Shared thread-safe targets remain supported.
// A throwing factory fails that operation and closes every member; callbacks
// already built retire on their own loops after their sessions, as at shutdown.
class http_server_group final {
public:
    http_server_group(std::span<const event_loop> loops, http_server_callbacks_factory callbacks, server_config config = {});
    ~http_server_group();
    http_server_group(const http_server_group&) = delete;
    http_server_group& operator=(const http_server_group&) = delete;
    http_server_group(http_server_group&&) = delete;
    http_server_group& operator=(http_server_group&&) = delete;

    // Any thread, before start only. start() loads and checks the staged
    // configuration, dispatch_ included.
    http_server_group& listen(listen_config config) &;
    http_server_group& listen(listen_config) && = delete;
    [[nodiscard]] task<prepared_listener> prepare_listener(worker_ref caller, listen_config config) &;
    task<prepared_listener> prepare_listener(worker_ref, listen_config) && = delete;
    // 整批所有 worker 就绪后才返回；单一票据统一激活或回滚全部端点。
    [[nodiscard]] task<prepared_listener> prepare_listeners(worker_ref caller, std::span<const listen_config> configs) &;
    task<prepared_listener> prepare_listeners(worker_ref, std::span<const listen_config>) && = delete;
    [[nodiscard]] task<void> add_listener(worker_ref caller, listen_config config) &;
    task<void> add_listener(worker_ref, listen_config) && = delete;
    [[nodiscard]] task<void> add_connection_service(worker_ref caller, listen_config config) &;
    task<void> add_connection_service(worker_ref, listen_config) && = delete;
    // Before start only; replaces the complete set on every worker. Parsing
    // and allocation finish before publication; invalid input changes nothing.
    // An empty config trusts no forwarded headers.
    http_server_group& trusted_proxies(trusted_proxy_config config) &;
    http_server_group& trusted_proxies(trusted_proxy_config) && = delete;
    // Ensures every member's callbacks are built, then prepares every worker
    // before opening any listener; failure rolls back the whole host without
    // cancelling unrelated caller work.
    [[nodiscard]] task<void> start(worker_ref caller) &;
    task<void> start(worker_ref) && = delete;
    [[nodiscard]] task<void> remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options = {}) &;
    task<void> remove_listener(worker_ref, const listener_endpoint&, listener_remove_options = {}) && = delete;
    // As http_server::wait_connections_closed(), across every member.
    [[nodiscard]] task<void> wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options = {}) &;
    task<void> wait_connections_closed(worker_ref, const listener_endpoint&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> replace_tls_identities(worker_ref caller, const listener_endpoint& endpoint, tls_config config) &;
    task<void> replace_tls_identities(worker_ref, const listener_endpoint&, tls_config) && = delete;
#ifndef _WIN32
    [[nodiscard]] task<listener_sockets> duplicate_listener_sockets(worker_ref caller, const listener_endpoint& endpoint) const&;
    task<listener_sockets> duplicate_listener_sockets(worker_ref, const listener_endpoint&) const&& = delete;
#endif
    // Explicit target worker; takes socket ownership even when rejected.
    // As http_server::adopt() otherwise.
    [[nodiscard]] task<void> adopt(worker_ref caller, std::size_t worker_index, const listener_endpoint& service, native_socket socket, adopt_options options = {}) &;
    task<void> adopt(worker_ref, std::size_t, const listener_endpoint&, native_socket, adopt_options = {}) && = delete;
    // IDs never name another connection after retirement.
    [[nodiscard]] task<bool> close_connection(worker_ref caller, std::uint64_t connection_id) &;
    task<bool> close_connection(worker_ref, std::uint64_t) && = delete;
    void close() & noexcept;
    void close() && = delete;
    [[nodiscard]] task<void> shutdown(worker_ref caller) &;
    task<void> shutdown(worker_ref) && = delete;
    // Any thread.
    [[nodiscard]] http_server_stats stats() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] const worker_ref& worker(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker(std::size_t) const&& = delete;

private:
    std::shared_ptr<detail::http_server_group_state> state_;
};

}  // namespace ruvia
