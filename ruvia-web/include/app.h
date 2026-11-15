#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "blocking_pool.h"
#include "borrowed_text.h"
#include "context.h"
#include "controller.h"
#include "env.h"
#include "http_client_types.h"
#include "http_error.h"
#include "http_header.h"
#include "http_server.h"
#include "middleware.h"
#include "rate_limiting.h"
#include "request_stream.h"
#include "server_connection.h"
#include "server_observation.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_state.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db_executor.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis_executor.h"
#endif

namespace ruvia {
namespace detail {

struct app_state;

}  // namespace detail

#ifdef RUVIA_ENABLE_DATABASE
struct db_registration_config final {
    borrowed_text alias_{"default"};
    db_config config_{};
    // Requires Redis support; without it, db() rejects a present value.
    std::optional<db_query_cache_registration> query_cache_{};
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct redis_registration_config final {
    borrowed_text alias_{"default"};
    redis_config config_{};
};
#endif

// Runtime listener controls (add_listener(), remove_listener(), ...) are the
// listener_controls members; they require a running app, and listeners they
// add or remove stay so for later run()s. stats() is all zero before run()
// starts the workers and after it returns, and is safe from a stop hook.
class application final : public listener_controls {
public:
    // Registers per-worker middleware instances in order before the matched
    // route's controller and route middleware. Arguments are retained at
    // registration; route model validators must be declared on their route.
    template <middleware middleware_type, typename... args_types>
    application& use(args_types&&... args) & {
        return use_middleware(detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
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
        return use_middleware(detail::make_scoped_app_middleware<middleware_type>(options, std::forward<args_types>(args)...));
    }
    template <middleware middleware_type, typename... args_types>
    application& use_at(const middleware_scope_options&, args_types&&...) && = delete;

    // One recipe per type; every worker owns an independent instance. A
    // request or posted job only borrows its own worker's instance.
    template <typename state_type, typename factory_type>
    application& use_worker_state(factory_type&& factory) & {
        return use_worker_state_definition(detail::worker_state_definition::make<state_type>(std::forward<factory_type>(factory)));
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
    application& document_root(document_root_config config) &;
    application& document_root(document_root_config config) && = delete;
    application& document_root(std::nullptr_t) &;
    application& document_root(std::nullptr_t) && = delete;
    // Configures the process-wide ordinary-thread pool every worker's event
    // loop borrows for worker_services::run_blocking(), file reads, large
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
    // configuration lock, as callbacks() does.
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
    // Replaces the whole host callback set (see http_server_callbacks); empty
    // members clear. Stopped app only (std::logic_error while running). The
    // previous set is destroyed after releasing the application configuration
    // lock, so a target's destructor may call stop() or query app state. A
    // listener whose dispatch_ needs an absent request_stream_ or connection_
    // fails run() with std::invalid_argument.
    application& callbacks(http_server_callbacks callbacks) &;
    application& callbacks(http_server_callbacks callbacks) && = delete;
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
    // configuration lock, as callbacks() does.
    application& on_start(std::nullptr_t) &;
    application& on_start(std::nullptr_t) && = delete;
    application& on_stop(std::nullptr_t) &;
    application& on_stop(std::nullptr_t) && = delete;
#ifdef RUVIA_ENABLE_DATABASE
    application& db(db_registration_config config) &;
    application& db(db_registration_config config) && = delete;
    application& db(std::nullptr_t) &;
    application& db(std::nullptr_t) && = delete;
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
    // The app's start + wait: binds and serves until stop(), then returns
    // after every worker retired (a host's start() then shutdown()). May run again
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
    explicit application(std::shared_ptr<detail::app_state> state);
    ~application();

    application(const application&) = delete;
    application& operator=(const application&) = delete;

    std::shared_ptr<detail::app_state> state_;
};

application& app();

}  // namespace ruvia
