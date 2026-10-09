// Operational middleware, readiness, proxy trust, logging, statistics, and hooks.
// Run ruvia_example_ops, then GET http://127.0.0.1:8080/admin/health,
// /admin/ready?db=down (503), /admin/stats, or /admin/peer.
// /admin/fail-after-head deliberately ends a stream with an error to demonstrate
// on_connection_failure; it cannot be turned into a second HTTP response.
// Set RUVIA_TRUST_LOCAL_PROXY=true only when a local proxy sanitizes forwarding
// headers. Ctrl+C closes admission, invokes the stop hook, and joins workers.

#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/health.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/security_headers.h"

#include "environment.h"

class ops_controller final : public ruvia::controller<ops_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/admin", ruvia::security_headers_middleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    // Configuration travels in the type, so a route-level middleware needs no
    // constructor arguments and no hand-written wrapper class.
    RUVIA_GET("/ready", ready, ruvia::rate_limit<10, 1000>);
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/peer", peer);
    RUVIA_GET_STREAM("/fail-after-head", fail_after_head);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> stats(ruvia::context& c) {
        const auto server = ruvia::app().http_stats();
        std::pmr::string text(c.arena());
        text.append("connections=").append(std::to_string(server.active_connections_));
        text.append(" refused=").append(std::to_string(server.connections_refused_));
        text.append(" failures=").append(std::to_string(server.connection_failures_));
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::task<ruvia::http_response> peer(ruvia::context& c) {
        const auto connection = c.conn();
        std::pmr::string text(c.arena());
        text.append("proxy=").append(connection.via_trusted_proxy() ? "trusted" : "none");
        // scheme() describes the client's connection, including an explicitly
        // trusted proxy. tls() describes only the transport to this server.
        text.append(" scheme=").append(connection.scheme() == ruvia::http_scheme::https ? "https" : "http");
        text.append(" local_tls=").append(connection.tls() ? "yes" : "no");
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::task<void> fail_after_head(ruvia::context& c) {
        co_await c.stream_text().write("the response has already started\n");
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

int main() {
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());
    if (env_value.get<bool>("RUVIA_TRUST_LOCAL_PROXY").value_or(false)) {
        app.trusted_proxies({.cidrs_ = {"127.0.0.1", "::1"}, .trust_x_forwarded_proto_ = true});
    }
    app
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        // Each worker counts independently. Route-level limits additionally
        // apply and can only tighten the deployment-wide rule.
        .get_rate_limit({.rule_ = {.max_requests_ = 100, .window_ = std::chrono::seconds(1)}})
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
                std::rethrow_exception(record.exception());
            } catch (const std::exception& error) {
                std::fprintf(stderr, "connection failed: %s\n", error.what());
            } catch (...) {
                std::fputs("connection failed with a non-standard exception\n", stderr);
            }
        })
        .run();
}
