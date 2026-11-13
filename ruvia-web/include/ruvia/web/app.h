#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/router.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/server_connection.h"
#include "ruvia/web/server_observation.h"
#include "ruvia/web/web_worker.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

namespace ruvia {

namespace detail {

struct app_state;

}  // namespace detail

#ifdef RUVIA_ENABLE_DATABASE
struct db_registration_config final {
    borrowed_text alias_{"default"};
    db_config config_{};
    // Requires Redis support; without it, database() rejects a present value.
    std::optional<db_query_cache_registration> query_cache_{};
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct redis_registration_config final {
    borrowed_text alias_{"default"};
    redis_config config_{};
};
#endif

class application final {
public:
    // Registers per-worker middleware instances in order before the matched
    // route's controller and route middleware. Arguments are retained at
    // registration; route model validators must be declared on their route.
    template <middleware middleware_type, typename... args_types>
    application& use(args_types&&... args) & {
        return use_middleware(
            detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }
    template <middleware middleware_type, typename... args_types>
    application& use(args_types&&...) && = delete;

    // Runs the middleware exactly on requests whose path is under the prefix:
    // whole path segments, compared percent-decoded, with trailing slashes
    // normalized ("/admin" covers "/admin", "/admin/x" and "/%61dmin/x", never
    // "/administrator"). Scope follows the request path, not the route
    // pattern: a route "/:section/panel" runs it for "/admin/panel" but not
    // "/other/panel". Membership is compiled into the route plan; only a route
    // that serves paths both inside and outside the scope checks the request
    // path at dispatch. Middleware constructor arguments cannot be confused
    // with the separately named scope. A "/" prefix throws
    // std::invalid_argument: use<T>() is the only app-wide spelling.
    template <middleware middleware_type, typename... args_types>
    application& use_at(const middleware_scope_options& options, args_types&&... args) & {
        return use_middleware(
            detail::make_scoped_app_middleware<middleware_type>(options, std::forward<args_types>(args)...));
    }
    template <middleware middleware_type, typename... args_types>
    application& use_at(const middleware_scope_options&, args_types&&...) && = delete;

    // One recipe per type; every worker owns an independent instance. A
    // request or posted job only borrows its own worker's instance.
    template <typename state_type, typename factory_type>
    application& use_worker_state(factory_type&& factory) & {
        return use_worker_state_definition(
            detail::worker_state_definition::make<state_type>(std::forward<factory_type>(factory)));
    }
    template <typename state_type, typename factory_type>
    application& use_worker_state(factory_type&&) && = delete;

    template <typename state_type>
    application& use_worker_state() & {
        return use_worker_state_definition(detail::make_default_worker_state<state_type>());
    }
    template <typename state_type>
    application& use_worker_state() && = delete;

    [[nodiscard]] const ruvia::env& env() const noexcept;
    application& load_dotenv(dotenv_options options = {}) &;
    application& load_dotenv(dotenv_options options = {}) && = delete;
    application& load_dotenv(const std::filesystem::path& path, dotenv_options options = {}) &;
    application& load_dotenv(const std::filesystem::path& path, dotenv_options options = {}) && = delete;
    application& server(server_config config) &;
    application& server(server_config config) && = delete;
    // Loads every TLS certificate, private key and client CA bundle of
    // config.tls_ once, here: unreadable files or a wrong key password throw
    // from listen(). The application keeps only the parsed credentials; this
    // call's copy of each private-key password and in-memory PEM text is
    // cleansed before it returns.
    application& listen(listen_config config) &;
    application& listen(listen_config config) && = delete;
    application& listen(std::span<const listen_config> configs) &;
    application& listen(std::span<const listen_config> configs) && = delete;
    application& listen(std::initializer_list<listen_config> configs) &;
    application& listen(std::initializer_list<listen_config> configs) && = delete;
    // Replaces the identities (identity_, sni_ and local_addresses_) of the
    // running HTTPS listener `endpoint` names, for TLS over TCP and its
    // HTTP/3 service alike, as listen() would load them; client_certificates_
    // (the same CA path and requirement) and max_outgoing_records_ must equal
    // the listener's. Credentials are prepared on caller's blocking pool,
    // then every worker adopts the new set on its own thread. Awaiting resumes
    // on caller, including when caller is an application worker.
    // Every handshake whose ClientHello a worker processes after completion
    // selects from the new set; established connections
    // and handshakes past their ClientHello keep their identity. Session
    // caches and ticket keys stay, so sessions keep resuming. The listener
    // keeps the new set for later run()s. An unknown endpoint, one without
    // HTTPS, a changed policy or an invalid setting throws
    // std::invalid_argument, and unreadable or mismatched credentials throw
    // as from listen(), all before anything changes. Throws std::logic_error
    // unless the app is running. When the app starts stopping
    // before every worker accepted the set, std::logic_error reports that
    // the replacement did not happen: the listener keeps its previous set for
    // later run()s, and workers that already adopted the new one serve it only
    // to handshakes racing the stop, which closes them. Concurrent calls take
    // effect in the order they publish. config's password and PEM copies are
    // owned immediately and cleansed on completion or destruction of the task.
    [[nodiscard]] task<void> replace_tls_identities(
        worker_ref caller, const listener_endpoint& endpoint, tls_config config) &;
    task<void> replace_tls_identities(
        worker_ref caller, const listener_endpoint& endpoint, tls_config config) && = delete;
    // Adds config's listeners to the running app, validated and loaded as
    // listen() would (an endpoint a listener already serves throws
    // std::invalid_argument), with any inherited_sockets_. Preparation uses
    // caller's blocking pool; owner threads bind or adopt prepared sockets,
    // and only once all succeeded does each start accepting. Awaiting joins
    // every accepted control and resumes on caller.
    // When any worker fails, every worker discards them, closing their
    // sockets, and the first failure is rethrown while the app serves as
    // before. The app keeps them for later run()s. Throws std::logic_error
    // unless the app is running, or when it starts stopping
    // before every worker accepted, and then they are not kept. config's
    // secrets are owned immediately and cleansed when the task completes or is dropped.
    [[nodiscard]] task<void> add_listener(worker_ref caller, listen_config config) &;
    task<void> add_listener(worker_ref caller, listen_config config) && = delete;
    // 与 add_listener 共用事务；全部 worker 已准备后返回未接入的 owning 句柄。
    [[nodiscard]] task<prepared_listener> prepare_listener(worker_ref caller, listen_config config) &;
    task<prepared_listener> prepare_listener(worker_ref caller, listen_config config) && = delete;
    // 同一业务配置的多个端点必须同批准备；任一失败均回滚整批，旧监听不变。
    // 调用时拥有化全部配置及继承 socket；返回后可释放输入 span 与借用地址。
    [[nodiscard]] task<prepared_listener> prepare_listeners(worker_ref caller, std::span<const listen_config> configs) &;
    task<prepared_listener> prepare_listeners(worker_ref caller, std::span<const listen_config> configs) && = delete;
    // Removes the running TCP service `endpoint` names (as for
    // replace_tls_identities()), with its HTTP/3 service. Awaiting completes
    // once every worker stopped admitting it, closed its TCP sockets and its
    // accept loop exited. Established connections are served until they end
    // or the app stops; an HTTP/3 service keeps receiving for its established
    // QUIC connections, ignoring new ones, and its UDP sockets close with the
    // last of them. Later run()s no longer serve it. An unknown endpoint throws
    // std::invalid_argument; the lifecycle rules of add_listener()
    // apply, and a removal the stop preempted keeps the listener for later
    // run()s. Concurrent runtime controls take effect one at a time.
    // close_connections_ 同时关闭目标监听的全部连接；会话由独立退休任务
    // join，不在本调用中等待（允许连接自己的处理器发起强制移除）。
    [[nodiscard]] task<void> remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options = {}) &;
    task<void> remove_listener(worker_ref caller, const listener_endpoint& endpoint, listener_remove_options options = {}) && = delete;
    // Waits for the serving or latest removed listener's TCP/TLS and QUIC
    // connections to reach zero. Cancellation/timeout leave connections alone;
    // stopping the app cancels the wait with operation_canceled.
    // Unknown endpoints throw std::invalid_argument; requires a running app.
    [[nodiscard]] task<void> wait_connections_closed(worker_ref caller, const listener_endpoint& endpoint, operation_options options = {}) &;
    task<void> wait_connections_closed(worker_ref, const listener_endpoint&, operation_options = {}) && = delete;
    // Closes every protocol stream of the connection; false means the ID is
    // no longer live. Application workers may await this control too.
    [[nodiscard]] task<bool> close_connection(worker_ref caller, std::uint64_t connection_id) &;
    task<bool> close_connection(worker_ref caller, std::uint64_t connection_id) && = delete;
#ifndef _WIN32
    // Duplicates (dup()) every worker's socket of the running TCP service
    // `endpoint` names, and of its HTTP/3 service, into http_ or https_ and
    // http3_ in worker order -- what a process exec'd to take over the
    // service passes as listen_config::inherited_sockets_. The duplicates are
    // not close-on-exec and belong to the caller; the app keeps serving on its
    // own sockets. QUIC connections do not move with the UDP sockets. An
    // unknown endpoint throws std::invalid_argument; the lifecycle
    // rules of add_listener() apply. POSIX only: on Windows the acceptor owns
    // a single exclusive socket per service.
    [[nodiscard]] task<listener_sockets> duplicate_listener_sockets(
        worker_ref caller, const listener_endpoint& endpoint) const&;
    task<listener_sockets> duplicate_listener_sockets(
        worker_ref caller, const listener_endpoint& endpoint) const&& = delete;
#endif
    application& document_root(document_root_config config) &;
    application& document_root(document_root_config config) && = delete;
    application& document_root(std::nullptr_t) &;
    application& document_root(std::nullptr_t) && = delete;
    // Configures the process-wide ordinary-thread pool every worker's event
    // loop borrows for context::run_blocking(), file reads, large
    // buffered-response compression, document-root refreshes and the DNS
    // lookups of registered clients. It is enabled by default with bounded
    // CPU-based sizing; nullptr explicitly disables it: run_blocking() then
    // reports blocking_status::unavailable, buffered responses above
    // sync_bytes_ stay identity (or 406 when identity is refused), a file
    // response is answered with 503 before its head instead of blocking a
    // worker on file I/O, and run() fails for a document root or for a
    // registered client whose host is not an IP literal. On
    // Windows plain TCP a single file range above 16 KiB still leaves through
    // TransmitFile, which needs no pool. run() starts the threads before the
    // first request; after workers stop, running callables are not awaited and
    // may finish on the pool's detached state.
    application& blocking_pool(blocking_pool_options config) &;
    application& blocking_pool(blocking_pool_options config) && = delete;
    application& blocking_pool(std::nullptr_t) &;
    application& blocking_pool(std::nullptr_t) && = delete;

    // The app-wide fallbacks; registering one a second time throws
    // std::invalid_argument instead of silently replacing the first.
    application& on_error(http_error_handler handler) &;
    application& on_error(http_error_handler handler) && = delete;
    application& on_not_found(http_not_found_handler handler) &;
    application& on_not_found(http_not_found_handler handler) && = delete;
    // Clearing retires the old handler after releasing the application
    // configuration lock, as on_access() does.
    application& on_error(std::nullptr_t) &;
    application& on_error(std::nullptr_t) && = delete;
    application& on_not_found(std::nullptr_t) &;
    application& on_not_found(std::nullptr_t) && = delete;
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // deepest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix") compared
    // percent-decoded; the prefix-less on_error/not_found remain the app-wide
    // fallback. A trailing slash is ignored; registering an equivalent prefix
    // twice ("/api", "/api/", "/%61pi") throws std::invalid_argument instead
    // of silently choosing by call order, and so does the prefix "/", which
    // would shadow the prefix-less form.
    application& on_error(scoped_error_handler_options options) &;
    application& on_error(scoped_error_handler_options options) && = delete;
    application& on_not_found(scoped_not_found_handler_options options) &;
    application& on_not_found(scoped_not_found_handler_options options) && = delete;
    // Peers whose forwarding headers name the real client. Accepts addresses
    // and CIDR blocks ("10.0.0.0/8", "2001:db8::/32", "127.0.0.1"); a malformed
    // entry throws std::invalid_argument at configuration time rather than
    // silently trusting nothing.
    //
    // Empty by default, and that default is the safe one: X-Forwarded-For is
    // client-controlled, so believing it from an arbitrary peer would let any
    // caller pick its own rate-limit key and claim a secure scheme. Configure
    // this ONLY with the addresses of proxies you operate. The client is the
    // last untrusted hop in the forwarding list, not the leftmost value the
    // caller can prepend. A hop that names no IP address (no RFC 7239 `for`,
    // `unknown`, an obfuscated name) ends the walk without naming a client:
    // client() is then the nearest trusted address after it, or remote().
    application& trusted_proxies(trusted_proxy_config config) &;
    application& trusted_proxies(trusted_proxy_config config) && = delete;

    // The rate limit every request passes. A route may add its own with
    // ruvia::rate_limit<max, window_ms>; both then apply, so the stricter is what
    // a caller actually meets -- the same "narrower scope may only tighten"
    // rule server_config::max_buffered_body_bytes_ follows.
    //
    // Worker-local: each worker counts independently, so a deployment with N
    // workers admits up to N times this rule. Size it accordingly.
    application& rate_limit(rate_limit_config config) &;
    application& rate_limit(rate_limit_config config) && = delete;
    application& rate_limit(std::nullptr_t) &;
    application& rate_limit(std::nullptr_t) && = delete;
    // Replacement/clearing retires the old target after releasing the application
    // configuration lock, so its destructor may call stop() or query app state.
    // The same retirement rule applies to on_connection_failure().
    application& on_access(access_log_callback callback) &;
    application& on_access(access_log_callback callback) && = delete;
    // Observes connections lost to an exception that escaped their session --
    // the failures on_error cannot answer because the response is already
    // committed or the error handler itself failed. Without a listener these
    // are reported to stderr; they are never silently dropped. A peer reset
    // or close, or I/O this server cancelled (stop, timeouts), is a
    // normal end of connection: it is neither delivered here nor counted.
    application& on_connection_failure(connection_failure_callback callback) &;
    application& on_connection_failure(connection_failure_callback callback) && = delete;
    // Observes every ClientHello a TLS listener receives, over TCP and for
    // HTTP/3, before certificate selection; a HelloRetryRequest yields a
    // second record. Without a callback the server installs no observer and
    // builds no record. Replacement and clearing follow on_access().
    application& on_tls_client_hello(tls_client_hello_callback callback) &;
    application& on_tls_client_hello(tls_client_hello_callback callback) && = delete;
    application& on_tls_client_hello(std::nullptr_t) &;
    application& on_tls_client_hello(std::nullptr_t) && = delete;
    // Observes the decrypted input of every HTTP/2 connection -- TLS with
    // ALPN h2 and cleartext prior knowledge alike -- once per read, before the
    // server parses it. Without a callback no record is built. Replacement
    // and clearing follow on_access().
    application& on_http2_input(http2_input_callback callback) &;
    application& on_http2_input(http2_input_callback callback) && = delete;
    application& on_http2_input(std::nullptr_t) &;
    application& on_http2_input(std::nullptr_t) && = delete;
#if defined(RUVIA_ENABLE_HTTP2)
    // Typed frame and HPACK facts from the protocol parser, in wire order.
    application& on_http2_frame(http2_frame_callback callback) &;
    application& on_http2_frame(http2_frame_callback callback) && = delete;
    application& on_http2_frame(std::nullptr_t) &;
    application& on_http2_frame(std::nullptr_t) && = delete;
    application& on_http2_headers(http2_headers_callback callback) &;
    application& on_http2_headers(http2_headers_callback callback) && = delete;
    application& on_http2_headers(std::nullptr_t) &;
    application& on_http2_headers(std::nullptr_t) && = delete;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    application& on_http3_frame(http3_frame_callback callback) &;
    application& on_http3_frame(http3_frame_callback callback) && = delete;
    application& on_http3_frame(std::nullptr_t) &;
    application& on_http3_frame(std::nullptr_t) && = delete;
    application& on_http3_field(http3_field_callback callback) &;
    application& on_http3_field(http3_field_callback callback) && = delete;
    application& on_http3_field(std::nullptr_t) &;
    application& on_http3_field(std::nullptr_t) && = delete;
#endif
    // Serves every listener_dispatch::connection listener (see
    // server_connection and connection_callback); run() throws
    // std::invalid_argument for such a listener without one. Replacement
    // and clearing follow on_access().
    application& on_connection(connection_callback callback) &;
    application& on_connection(connection_callback callback) && = delete;
    application& on_connection(std::nullptr_t) &;
    application& on_connection(std::nullptr_t) && = delete;
    // Admission precedes TLS/HTTP processing. Open may return owned per-
    // connection state; close runs after retirement on the owning worker.
    application& on_connection_admission(connection_admission_callback callback) &;
    application& on_connection_admission(connection_admission_callback callback) && = delete;
    application& on_connection_admission(std::nullptr_t) &;
    application& on_connection_admission(std::nullptr_t) && = delete;
    application& on_connection_open(connection_open_callback callback) &;
    application& on_connection_open(connection_open_callback callback) && = delete;
    application& on_connection_open(std::nullptr_t) &;
    application& on_connection_open(std::nullptr_t) && = delete;
    application& on_connection_close(connection_close_callback callback) &;
    application& on_connection_close(connection_close_callback callback) && = delete;
    application& on_connection_close(std::nullptr_t) &;
    application& on_connection_close(std::nullptr_t) && = delete;
    // Serves every listener_dispatch::request_stream listener (see
    // request_stream and request_stream_callback); run() throws
    // std::invalid_argument for such a listener without one. Replacement
    // and clearing follow on_access().
    application& on_request_stream(request_stream_callback callback) &;
    application& on_request_stream(request_stream_callback callback) && = delete;
    application& on_request_stream(std::nullptr_t) &;
    application& on_request_stream(std::nullptr_t) && = delete;
    // Runs on the run() caller after worker capabilities are ready, before
    // TCP admission. All hooks must succeed before serving; failure or a
    // stop request rolls back startup. Ready workers can execute posted jobs.
    application& on_start(app_hook hook) &;
    application& on_start(app_hook hook) && = delete;
    // Runs on the run() caller once every worker was told to stop -- admission
    // closed and its connections closing, with handlers possibly still
    // unwinding -- before runtime join. Also runs when published startup is
    // cancelled or a start hook fails. A hook must not throw: a failure is
    // reported, not rethrown.
    application& on_stop(app_hook hook) &;
    application& on_stop(app_hook hook) && = delete;
    // Clearing retires the old hooks after releasing the application
    // configuration lock, as on_access() does.
    application& on_start(std::nullptr_t) &;
    application& on_start(std::nullptr_t) && = delete;
    application& on_stop(std::nullptr_t) &;
    application& on_stop(std::nullptr_t) && = delete;
#ifdef RUVIA_ENABLE_DATABASE
    application& database(db_registration_config config) &;
    application& database(db_registration_config config) && = delete;
    application& database(std::nullptr_t) &;
    application& database(std::nullptr_t) && = delete;
#endif
#ifdef RUVIA_ENABLE_REDIS
    application& redis(redis_registration_config config) &;
    application& redis(redis_registration_config config) && = delete;
    application& redis(std::nullptr_t) &;
    application& redis(std::nullptr_t) && = delete;
#endif
    application& client(http_client_config config) &;
    application& client(http_client_config config) && = delete;
    application& client(std::nullptr_t) &;
    application& client(std::nullptr_t) && = delete;
    // Serves until stop(); returns after every worker retired. May run again
    // after it returns. The first preparation, startup, or worker failure is
    // rethrown to the caller, who is responsible for printing it; run() does
    // not report it. Failures it supersedes are reported to stderr, with
    // equivalent worker failures collapsed into one line with their count.
    void run() &;
    void run() && = delete;
    // Safe from any thread. A stop() issued while the app is not running is
    // kept for the next run(), which then returns without serving. A stop
    // during startup rolls it back at once. A serving app stops by direct
    // disconnect, with no drain phase: admission stops (pending accepts and
    // QUIC receives are cancelled), then every connection is closed at once
    // -- HTTP/1, HTTP/2, WebSocket and tunnel sockets close without a final
    // response, GOAWAY or close handshake, and each HTTP/3 connection sends
    // CONNECTION_CLOSE(H3_NO_ERROR) -- and handlers observe their stop token
    // (context::stop_token()); then worker I/O stops before background
    // operations are joined.
    void stop() &;
    void stop() && = delete;
    // HTTP serving counters summed across every worker. All zero before run()
    // starts them and after it returns. Safe from any thread, including from a
    // stop hook.
    [[nodiscard]] http_server_stats stats() const;
    // The blocking pool's counters, or all zero when no pool is configured or
    // the app is not running. Queue depth and the rejected count are what a
    // deployment sizes thread_count/queue_capacity from. Safe from any thread.
    [[nodiscard]] ::ruvia::blocking_pool_stats blocking_pool_stats() const;
    // Worker handles, published once every worker is ready (before on_start
    // hooks run) and until run() returns. Empty or invalid otherwise.
    [[nodiscard]] std::vector<web_worker_ref> workers() const;
    [[nodiscard]] web_worker_ref worker_for(std::uint64_t key) const;
    [[nodiscard]] web_worker_ref worker_for(std::string_view key) const;

private:
    friend application& app();

    application& use_middleware(detail::owned_middleware_descriptor descriptor) &;
    application& use_worker_state_definition(detail::worker_state_definition definition) &;

    application();
    ~application();

    application(const application&) = delete;
    application& operator=(const application&) = delete;

    std::shared_ptr<detail::app_state> state_;
};

application& app();

}  // namespace ruvia
