#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/web/app_hook.h"
#include "ruvia/web/detail/app/app_configuration.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/error_handlers.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/rate_limit_rule.h"
#include "ruvia/web/server_config.h"
#include "ruvia/web/web_worker.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_cache.h"
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
    std::string alias_{"default"};
    db_config config_{};
#ifdef RUVIA_ENABLE_REDIS
    std::optional<db_query_cache_registration> query_cache_{};
#endif
};
#endif

#ifdef RUVIA_ENABLE_REDIS
struct redis_registration_config final {
    std::string alias_{"default"};
    redis_config config_{};
};
#endif

struct http_client_registration_config final {
    std::string alias_{"default"};
    http_client_config config_{};
};

class application final {
public:
    // Registers per-worker middleware instances in order before the matched
    // route's controller and route middleware. Arguments are retained at
    // registration; route model validators must be declared on their route.
    template <middleware middleware_type, typename... args_types>
    application& use(args_types&&... args) {
        return use_middleware(
            detail::make_owned_middleware_descriptor<middleware_type>(std::forward<args_types>(args)...));
    }

    // Runs the middleware exactly on requests whose path is under the prefix:
    // whole path segments, compared percent-decoded, with trailing slashes
    // normalized ("/admin" covers "/admin", "/admin/x" and "/%61dmin/x", never
    // "/administrator"). Scope follows the request path, not the route
    // pattern: a route "/:section/panel" runs it for "/admin/panel" but not
    // "/other/panel". Membership is compiled into the route plan; only a route
    // that serves paths both inside and outside the scope checks the request
    // path at dispatch. Middleware constructor arguments cannot be confused
    // with the separately named scope.
    template <middleware middleware_type, typename... args_types>
    application& use_at(const middleware_scope_options& options, args_types&&... args) {
        return use_middleware(
            detail::make_scoped_app_middleware<middleware_type>(options, std::forward<args_types>(args)...));
    }

    // One recipe per type; every worker owns an independent instance. A
    // request or posted job only borrows its own worker's instance.
    template <typename state_type, typename factory_type>
    application& use_worker_state(factory_type&& factory) {
        return use_worker_state_definition(
            detail::worker_state_definition::make<state_type>(std::forward<factory_type>(factory)));
    }

    template <typename state_type>
    application& use_worker_state() {
        return use_worker_state_definition(detail::make_default_worker_state<state_type>());
    }

    [[nodiscard]] const ruvia::env& env() const noexcept;
    application& load_dotenv(dotenv_options options = {});
    application& load_dotenv(const std::filesystem::path& path, dotenv_options options = {});
    application& server(server_config config);
    // Loads every TLS certificate, private key and client CA bundle of
    // config.tls_ once, here: unreadable files or a wrong key password throw
    // from listen(). The application keeps only the parsed credentials; this
    // call's copy of each private-key password is cleansed before it returns.
    application& listen(listen_config config);
    // Disabled by default. A config enables response coding and precompressed
    // static variant negotiation; nullptr disables both.
    application& compression(compression_config config);
    application& compression(std::nullptr_t);
    application& cors(cors_config config);
    application& cors(std::nullptr_t);
    application& document_root(document_root_config config);
    application& document_root(std::nullptr_t);
    // Configures the process-wide ordinary-thread pool used by
    // context::run_blocking() and large buffered-response compression. It is
    // enabled by default with bounded CPU-based sizing; nullptr explicitly
    // disables it, making large buffered-response compression synchronous.
    // run() starts the threads before the first request; after workers stop,
    // running callables are not awaited and may finish on the pool's detached
    // state.
    application& get_blocking_pool(blocking_pool_options config);
    application& get_blocking_pool(std::nullptr_t);

    application& on_error(http_error_handler_type handler);
    application& on_not_found(http_not_found_handler_type handler);
    // Path-prefix-scoped fallbacks, the Hono sub-app scoping analog: the
    // deepest matching registered prefix wins, matching on whole path
    // segments ("/api" scopes "/api" and "/api/x", never "/apix") compared
    // percent-decoded; the prefix-less on_error/not_found remain the app-wide
    // fallback. A trailing slash is ignored; registering an equivalent prefix
    // twice ("/api", "/api/", "/%61pi") throws std::invalid_argument instead
    // of silently choosing by call order.
    application& on_error(scoped_error_handler_options options);
    application& on_not_found(scoped_not_found_handler_options options);
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
    application& trusted_proxies(trusted_proxy_config config);
    application& trusted_proxies(std::nullptr_t);

    // The rate limit every request passes. A route may add its own with
    // ruvia::rate_limit<max, window_ms>; both then apply, so the stricter is what
    // a caller actually meets -- the same "narrower scope may only tighten"
    // rule server_config::max_buffered_body_bytes_ follows.
    //
    // Worker-local: each worker counts independently, so a deployment with N
    // workers admits up to N times this rule. Size it accordingly.
    application& get_rate_limit(rate_limit_config config);
    application& get_rate_limit(std::nullptr_t);
    application& on_access(access_log_callback_type callback);
    // Observes connections lost to an exception that escaped their session --
    // the failures on_error cannot answer because the response is already
    // committed or the error handler itself failed. Without a listener these
    // are reported to stderr; they are never silently dropped. A peer reset
    // or close, or I/O this server cancelled (stop, drain, timeouts), is a
    // normal end of connection: it is neither delivered here nor counted.
    application& on_connection_failure(connection_failure_callback_type callback);
    // Runs on the run() caller after worker capabilities are ready, before
    // TCP admission. All hooks must succeed before serving; failure or a
    // stop request rolls back startup. Ready workers can execute posted jobs.
    application& on_start(app_hook_type hook);
    // Runs on the run() caller after admission closes and in-flight requests
    // have drained (or server_config::drain_timeout_ passed), before runtime
    // join. Also runs when published startup is cancelled or a start hook fails.
    application& on_stop(app_hook_type hook);
#ifdef RUVIA_ENABLE_DATABASE
    application& database(db_registration_config config);
    application& database(std::nullptr_t);
#endif
#ifdef RUVIA_ENABLE_REDIS
    application& redis(redis_registration_config config);
    application& redis(std::nullptr_t);
#endif
    application& get_http_client(http_client_registration_config config);
    application& get_http_client(std::nullptr_t);
    // Serves until stop(); returns after every worker retired. May run again
    // after it returns. The first preparation, startup, or worker failure is
    // rethrown to the caller, who is responsible for printing it; run() does
    // not report it. Failures it supersedes are reported to stderr, with
    // equivalent worker failures collapsed into one line with their count.
    void run();
    // Safe from any thread. A stop() issued while the app is not running is
    // kept for the next run(), which then returns without serving. A stop
    // during startup rolls it back at once. A serving app stops gracefully:
    // listeners close; connections without an admitted request (idle, or still
    // receiving a request head) close; each WebSocket session starts its close
    // handshake with 1001 Going Away, bounded by its close_handshake_timeout_;
    // admitted requests and tunnels may finish, each request closing its
    // connection after the response. Once server_config::drain_timeout_
    // passes, every remaining connection is closed and handlers observe their
    // stop token. A worker failure skips the drain.
    void stop();
    // HTTP serving counters summed across every worker. All zero before run()
    // starts them and after it returns. Safe from any thread, including from a
    // stop hook.
    [[nodiscard]] http_server_stats http_stats() const;
    // The blocking pool's counters, or all zero when no pool is configured or
    // the app is not running. Queue depth and the rejected count are what a
    // deployment sizes thread_count/queue_capacity from. Safe from any thread.
    [[nodiscard]] blocking_pool_stats get_blocking_pool_stats() const;
    // Worker handles, published once every worker is ready (before on_start
    // hooks run) and until run() returns. Empty or invalid otherwise.
    [[nodiscard]] std::vector<web_worker_handle> workers() const;
    [[nodiscard]] web_worker_handle worker_for(std::uint64_t key) const;
    [[nodiscard]] web_worker_handle worker_for(std::string_view key) const;

private:
    friend application& app();

    application& use_middleware(detail::owned_middleware_descriptor descriptor);
    application& use_worker_state_definition(detail::worker_state_definition definition);

    struct state_deleter_type {
        void operator()(detail::app_state* state) const noexcept;
    };

    application();
    ~application();

    application(const application&) = delete;
    application& operator=(const application&) = delete;

    std::unique_ptr<detail::app_state, state_deleter_type> state_;
};

application& app();

}  // namespace ruvia
