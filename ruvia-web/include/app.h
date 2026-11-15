#pragma once

#include <algorithm>
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
#include <thread>
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
#include "move_only_function.h"
#include "rate_limiting.h"
#include "request_stream.h"
#include "server_connection.h"
#include "server_observation.h"
#include "static_root.h"
#include "task.h"
#include "timeout_duration.h"
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

enum class process_signal_handler_policy : std::uint8_t {
    external_owner,
    install,
};

// The workers application::run() owns. Standalone hosts (http_server,
// http_server_group) run on the caller's event loops and have no counterpart.
struct application_worker_config final {
    // Business workers, each with its own event loop; a connection stays on
    // the worker that adopted it. On POSIX every worker binds each TCP
    // listener with SO_REUSEPORT and accepts on its own event loop. On
    // Windows one acceptor thread per application owns every listener
    // exclusively (SO_EXCLUSIVEADDRUSE) and hands each accepted socket to the
    // business worker holding the fewest connections; a socket that worker's
    // queue cannot admit is closed and counted in connections_refused_.
    std::size_t worker_count_{(std::max)(1U, std::thread::hardware_concurrency())};
    // install: run() installs SIGINT/SIGTERM handlers that call stop().
    process_signal_handler_policy process_signal_handlers_{process_signal_handler_policy::external_owner};
    // Bound of each worker's cross-thread job queue.
    std::size_t worker_queue_capacity_{1024};
};

// Runtime behavior belongs to the server's document-root binding, not to the
// immutable static_root index. application document roots are always refreshed:
// one refresher per application rebuilds the index each interval and every
// worker adopts the shared snapshot. A standalone static_root remains immutable
// because it has no server runtime.
struct document_root_config final {
    std::filesystem::path root_{};
    static_root_options static_options_{};
    timeout_duration refresh_interval_{std::chrono::seconds(1)};
    bool precompress_gzip_{false};
    bool precompress_brotli_{false};
    bool precompress_zstd_{false};
    std::size_t precompress_min_bytes_{1024};
    std::size_t precompress_max_bytes_{std::size_t{256} * 1024};
};

// A self-contained startup/shutdown callback, invoked on the application::run() caller.
// on_start runs after worker initialization, before TCP admission. on_stop
// runs after admission closes, before runtime join (including start-hook failure
// or cancellation). A self-contained callable is owned by the hook value;
// references captured by that callable must still
// outlive the registered hook. Invoking an empty hook is a programming error
// and throws std::logic_error.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using app_hook = move_only_function<void()>;

// Runtime listener controls (add_listener(), remove_listener(), ...) are the
// listener_controls members; they require a running app, and listeners they
// add or remove stay so for later run()s. stats() is all zero before run()
// starts the workers and after it returns, and is safe from a stop hook.
// Middleware, worker state, fallback handlers and the rate limit are the
// dispatch_configuration members; they require a stopped app.
class application final : public listener_controls, public dispatch_configuration<application> {
public:
    [[nodiscard]] const ruvia::env& env() const noexcept;
    application& load_dotenv(dotenv_options options = {}) &;
    application& load_dotenv(dotenv_options options = {}) && = delete;
    application& load_dotenv(const std::filesystem::path& path, dotenv_options options = {}) &;
    application& load_dotenv(const std::filesystem::path& path, dotenv_options options = {}) && = delete;
    // Validated as one unit; a stopped app only, like every setter below.
    application& workers(application_worker_config config) &;
    application& workers(application_worker_config config) && = delete;
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
    // loop borrows for worker_services_access::run_blocking(), file reads, large
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
    application& trusted_proxies(std::nullptr_t) &;
    application& trusted_proxies(std::nullptr_t) && = delete;
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
    friend class dispatch_configuration<application>;

    // The dispatch_configuration guard: the app lock, and a stopped app.
    void edit_configuration(detail::callback_ref<void(detail::app_configuration&)> edit);

    application();
    explicit application(std::shared_ptr<detail::app_state> state);
    ~application();

    application(const application&) = delete;
    application& operator=(const application&) = delete;

    std::shared_ptr<detail::app_state> state_;
};

application& app();

}  // namespace ruvia
