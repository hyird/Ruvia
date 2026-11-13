// Runtime configuration: dotenv, app-wide middleware via application::use, memory
// pool, timeouts, limits, compression and optional TLS. Typed Env::get<T>()
// returns nullopt only when a variable is absent; malformed values fail fast.
// Run ruvia_example_runtime_config and GET http://127.0.0.1:8087/runtime.
// RUVIA_HTTP_PORT/PORT and RUVIA_WORKERS override the listener and worker count.
// RUVIA_GZIP=true enables negotiated gzip/Brotli/zstd; RUVIA_CORS=true enables
// the CORS policy below. A .env file is optional and preserves existing values.
// For HTTPS, set RUVIA_TLS_CERT/KEY and optionally RUVIA_HTTPS_PORT (8443).
// RUVIA_TLS_CLIENT_CA supplies a client CA; RUVIA_REQUIRE_CLIENT_CERT=true
// requires mTLS. SNI uses RUVIA_SNI_HOST/CERT/KEY. RUVIA_AUTO_HTTPS=true redirects
// plain HTTP. Never replace certificate verification with an insecure policy.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "backend_config.h"
namespace {

void assign_if_present(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

}  // namespace

// Registered app-wide below (application::use): one instance per worker runs before the
// controller and route middlewares of EVERY matched route, in use() order.
// Requests that match no route (404/405) skip it: only a middleware declaring
// `static constexpr bool ruvia_runs_on_unmatched_requests = true;` wraps those.
class global_header_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Runtime-Example", "true");
    }
};

class scoped_header_middleware final {
public:
    explicit scoped_header_middleware(std::string_view value)
        : value_(value) {}

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Scoped-Example", value_);
    }

private:
    std::string value_;
};

class runtime_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/runtime")
    RUVIA_ROUTES_BEGIN(runtime_controller)
    RUVIA_GET("/", runtime);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> runtime(ruvia::context& c) {
        co_return c.text("runtime configured\n");
    }
};

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        app.use<global_header_middleware>();
        // Runs on requests whose path is under /runtime; scope membership is
        // compiled into the route plan. Constructor arguments are owned by the
        // registration and materialized separately on each worker.
        app.use_at<scoped_header_middleware>({.prefix_ = "/runtime"}, "configured");

        const auto http_port = env_value.get<std::uint16_t>("RUVIA_HTTP_PORT")
                                   .value_or(env_value.get<std::uint16_t>("RUVIA_PORT").value_or(8087));
        std::optional<ruvia::compression_config> compression;
        if (env_value.get<bool>("RUVIA_GZIP").value_or(false)) {
            compression.emplace();
        }
        std::optional<ruvia::http_cors_policy_config> cors;
        if (env_value.get<bool>("RUVIA_CORS").value_or(false)) {
            ruvia::http_cors_policy_config policy;
            policy.allowed_headers_ = {"content-type", "authorization"};
            policy.max_age_ = std::chrono::seconds(600);
            cors = std::move(policy);
        }
        app.server({
            .worker_count_ = env_value.get<std::uint32_t>("RUVIA_WORKERS").value_or(2),
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .idle_timeout_ = std::chrono::seconds(75),
            .connection_scan_interval_ = std::chrono::seconds(1),
            .request_header_timeout_ = std::chrono::seconds(60),
            .request_body_timeout_ = std::chrono::seconds(60),
            .write_timeout_ = std::chrono::seconds(60),
            .max_connections_per_worker_ = 10000,
            .max_requests_per_connection_ = 1000,
            .max_buffered_body_bytes_ = 16 * 1024 * 1024,
            .max_websocket_message_bytes_ = 16 * 1024 * 1024,
            .compression_ = std::move(compression),
            .cors_ = std::move(cors),
        });

        const auto cert = env_value.path("RUVIA_TLS_CERT");
        const auto key = env_value.path("RUVIA_TLS_KEY");
        if (cert && key) {
            std::string password;
            assign_if_present(password, env_value.get("RUVIA_TLS_PASSWORD"));
            ruvia::listen_config listener_value{
                .address_ = "0.0.0.0",
                .http_ = http_port,
                .https_ = env_value.get<std::uint16_t>("RUVIA_HTTPS_PORT").value_or(8443),
                .tls_ =
                    {
                        .identity_ =
                            {
                                .certificate_chain_file_ = *cert,
                                .private_key_file_ = *key,
                                .private_key_password_ = password,
                            },
                    },
                .auto_https_redirect_ = env_value.get<bool>("RUVIA_AUTO_HTTPS").value_or(false),
            };
            if (const auto client_ca = env_value.path("RUVIA_TLS_CLIENT_CA")) {
                listener_value.tls_.client_certificates_.ca_file_ = *client_ca;
                listener_value.tls_.client_certificates_.requirement_ =
                    env_value.get<bool>("RUVIA_REQUIRE_CLIENT_CERT").value_or(false)
                        ? ruvia::tls_client_certificate_requirement::required
                        : ruvia::tls_client_certificate_requirement::optional;
            }
            if (const auto host = env_value.get("RUVIA_SNI_HOST")) {
                // Each SNI entry has its own certificate/key. The default listener
                // certificate remains the fallback for other server names.
                listener_value.tls_.sni_.push_back({
                    .host_ = std::string(*host),
                    .identity_ =
                        {
                            .certificate_chain_file_ = env_value.path("RUVIA_SNI_CERT").value_or(std::filesystem::path{}),
                            .private_key_file_ = env_value.path("RUVIA_SNI_KEY").value_or(std::filesystem::path{}),
                        },
                });
            }
            app.listen(std::move(listener_value));
        } else {
            app.listen({
                .address_ = "0.0.0.0",
                .http_ = http_port,
            });
        }

        app.run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}

// Operational middleware, readiness, proxy trust, logging, statistics, and hooks.
// Run ruvia_example_ops, then GET http://127.0.0.1:8098/admin/health,
// /admin/ready?db=down (503), /admin/stats, or /admin/peer.
// /admin/fail-after-head deliberately ends a stream with an error to demonstrate
// on_connection_failure; it cannot be turned into a second HTTP response.
// Set RUVIA_TRUST_LOCAL_PROXY=true only when a local proxy sanitizes forwarding
// headers. Ctrl+C closes admission, invokes the stop hook, and joins workers.

class ops_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/admin", ruvia::security_headers_middleware)

    RUVIA_ROUTES_BEGIN(ops_controller)
    RUVIA_GET("/health", health);
    // Configuration travels in the type, so a route-level middleware needs no
    // constructor arguments and no hand-written wrapper class.
    RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/peer", peer);
    RUVIA_GET("/fail-after-head", fail_after_head);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        const auto server = ruvia::app().stats();
        std::string text;
        text.append("connections=").append(std::to_string(server.active_connections_));
        text.append(" refused=").append(std::to_string(server.connections_refused_));
        text.append(" failures=").append(std::to_string(server.connection_failures_));
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::task<ruvia::http_response> peer(ruvia::context& c) {
        const auto connection = c.conn();
        std::string text;
        text.append("proxy=").append(connection.via_trusted_proxy() ? "trusted" : "none");
        // scheme() describes the client's connection, including an explicitly
        // trusted proxy. tls() describes only the transport to this server.
        text.append(" scheme=").append(connection.scheme() == ruvia::http_scheme::https ? "https" : "http");
        text.append(" local_tls=").append(connection.tls() ? "yes" : "no");
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::task<void> fail_after_head(ruvia::context& c) {
        if (co_await c.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("the response has already started\n");
        throw std::runtime_error("example streaming producer failed");
    }

    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return ruvia::make_health_response(c);
    }

    ruvia::task<ruvia::http_response> ready(ruvia::context& c) {
        const bool database_ready = c.req().query("db").value_or("up") != "down";
        co_return ruvia::make_readiness_response(
            c, {
                   .state_ = database_ready ? ruvia::readiness_state::ready
                                            : ruvia::readiness_state::unavailable,
                   .unavailable_reason_ = "database is not ready",
               });
    }
};

[[maybe_unused]] int run_ops_main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        if (env_value.get<bool>("RUVIA_TRUST_LOCAL_PROXY").value_or(false)) {
            app.trusted_proxies({.cidrs_ = {"127.0.0.1", "::1"}, .trust_x_forwarded_proto_ = true});
        }
        app
            .listen({.address_ = "0.0.0.0", .http_ = 8098})
            .server({.worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            // Each worker counts independently. Route-level limits additionally
            // apply and can only tighten the deployment-wide rule.
            .rate_limit({.rule_ = {.max_requests_ = 100, .window_ = std::chrono::seconds(1)}})
            .on_start([] {
                // Runs on the thread calling run(), after worker capabilities are
                // ready and before any listener starts serving. Throwing rolls back
                // startup; wait for any posted initialization before returning.
                std::puts("all workers ready; opening admission");
            })
            .on_stop([] {
                // Stop external producers here. Worker teardown is joined after
                // this hook; other threads should only request application::stop().
                std::puts("admission closed; joining workers");
            })
            .on_access([](const ruvia::access_log_record& record) noexcept {
                // Views are callback-scoped. Production sinks should enqueue an
                // owned record to a bounded logger instead of blocking the worker.
                std::printf("%.*s %.*s in %llu us\n",
                    static_cast<int>(record.method().size()), record.method().data(),
                    static_cast<int>(record.path().size()), record.path().data(),
                    static_cast<unsigned long long>(record.duration_micros()));
            })
            .on_connection_failure([](const ruvia::connection_failure_record& record) noexcept {
                try {
                    std::fprintf(stderr, "connection failed: %s\n", ruvia::exception_text(record.exception()).c_str());
                } catch (...) {
                    std::fputs("connection failed; formatting its reason failed\n", stderr);
                }
            })
            .run();
    } catch (...) {
        std::fprintf(stderr, "%s\n", ruvia::exception_text(std::current_exception()).c_str());
        return 1;
    }
    return 0;
}

// Worker-local state and blocking work: use_worker_state<T> per-worker
// instances shared by the HTTP and dispatch paths, cross-worker task posting
// via application::worker_for / web_worker_ref::post, and context::run_blocking() --
// the escape hatch for calls that block, which run on application::blocking_pool()'s
// threads so the worker stays free to serve its other connections.
// Run ruvia_example_workers_blocking on port 8090; inspect its route macros
// for stats, awaited dispatch, fanout and overload shedding endpoints.
// Offloaded work takes owned input. Do not capture context or worker state
// in a blocking job: stopping a request does not stop that native thread.

namespace workers_blocking_example {

// One instance per worker, touched only from that worker's thread: no locks.
struct worker_stats final {
    std::uint64_t served_{0};
    std::uint64_t dispatched_{0};
};

// Stand-in for a genuinely blocking call (sync SDK, file crypto, ...).
std::uint64_t slow_checksum(std::string_view input) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::uint64_t sum = 14695981039346656037ull;
    for (const char byte : input) {
        sum = (sum ^ static_cast<unsigned char>(byte)) * 1099511628211ull;
    }
    return sum;
}

class worker_tools_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/workers")
    RUVIA_ROUTES_BEGIN(worker_tools_controller)
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/offload", offload);
    RUVIA_GET("/offload-or-shed", offload_or_shed);
    RUVIA_GET("/fanout", fanout);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        auto& state_value = c.worker_state<worker_stats>();
        ++state_value.served_;
        std::string body;
        body.append("served=");
        body.append(std::to_string(state_value.served_));
        body.append(" dispatched=");
        body.append(std::to_string(state_value.dispatched_));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Blocking escape hatch. The callable runs on a pool thread, so it must own
    // what it touches: the query value is copied into the lambda, never
    // borrowed from the request. The handler suspends and resumes on its own
    // worker, which kept serving other connections meanwhile. A saturated pool
    // becomes a typed rejection; take_value() explicitly throws
    // blocking_operation_rejected, which the default error path answers with 503.
    ruvia::task<ruvia::http_response> offload(ruvia::context& c) {
        auto result_value = co_await c.run_blocking(
            [input = std::string(c.req().query("input").value_or("default"))] {
                return slow_checksum(input);
            });
        const auto checksum = std::move(result_value).take_value();
        std::string body;
        body.append("checksum=");
        body.append(std::to_string(checksum));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // The same work, with the overload answered by this handler instead of by
    // the error path, and a deadline so one wedged call cannot pin the request
    // forever.
    ruvia::task<ruvia::http_response> offload_or_shed(ruvia::context& c) {
        auto result_value = co_await c.run_blocking(
            [input = std::string("expensive input")] { return slow_checksum(input); },
            {.timeout_ = std::chrono::seconds(2)});
        if (!result_value.completed()) {
            co_return c.error({.status_ = ruvia::http_status::service_unavailable,
                .code_ = "busy",
                .message_ = ruvia::describe_blocking_status(result_value.status())});
        }
        std::string body;
        body.append("checksum=");
        body.append(std::to_string(std::move(result_value).take_value()));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    // Cross-worker dispatch: pick a worker by key and post a job onto its
    // event loop. The job sees the SAME per-worker state instance that
    // worker's HTTP requests see.
    ruvia::task<ruvia::http_response> fanout(ruvia::context& c) {
        std::size_t accepted = 0;
        for (const auto& worker : ruvia::app().workers()) {
            const auto posted = worker.post([](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
                ++ctx.worker_state<worker_stats>().dispatched_;
                co_return;
            });
            if (posted == ruvia::post_status::accepted) {
                ++accepted;
            }
        }
        std::string body;
        body.append("posted=");
        body.append(std::to_string(accepted));
        body.push_back('\n');
        co_return c.text(std::move(body));
    }
};

}  // namespace workers_blocking_example

[[maybe_unused]] int run_workers_blocking_main() {
    using namespace workers_blocking_example;
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8090})
            .server({.worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            // Each worker builds its own worker_stats before serving; the factory
            // form (use_worker_state<T>(fn)) covers non-default-constructible types.
            .use_worker_state<worker_stats>()
            // One pool for the whole process, shared by every worker. application provides
            // a bounded pool by default; this overrides its size for the example.
            // application::blocking_pool_stats() reports queue depth and rejections for
            // production tuning.
            .blocking_pool({
                .thread_count_ = 4,
                .queue_capacity_ = 128,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
