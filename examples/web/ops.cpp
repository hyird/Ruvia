// Operational middleware, readiness, proxy trust, logging, statistics, and hooks.
// Run ruvia_example_ops, then GET http://127.0.0.1:8080/admin/health,
// /admin/ready?db=down (503), /admin/stats, or /admin/peer.
// /admin/fail-after-head deliberately ends a stream with an error to demonstrate
// onConnectionFailure; it cannot be turned into a second HTTP response.
// Set RUVIA_TRUST_LOCAL_PROXY=true only when a local proxy sanitizes forwarding
// headers. Ctrl+C closes admission, invokes the stop hook, and joins workers.

#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Health.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/SecurityHeaders.h"

#include "environment.h"

class OpsController final : public ruvia::Controller<OpsController> {
public:
    RUVIA_CONTROLLER_GROUP("/admin", ruvia::SecurityHeadersMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    // Configuration travels in the type, so a route-level middleware needs no
    // constructor arguments and no hand-written wrapper class.
    RUVIA_GET("/ready", ready, ruvia::RateLimit<10, 1000>);
    RUVIA_GET("/stats", stats);
    RUVIA_GET("/peer", peer);
    RUVIA_GET_STREAM("/fail-after-head", fail_after_head);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> stats(ruvia::Context& c) {
        const auto server = ruvia::app().httpStats();
        std::pmr::string text(c.arena());
        text.append("connections=").append(std::to_string(server.activeConnections));
        text.append(" refused=").append(std::to_string(server.connectionsRefused));
        text.append(" failures=").append(std::to_string(server.connectionFailures));
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::Task<ruvia::HttpResponse> peer(ruvia::Context& c) {
        const auto connection = c.conn();
        std::pmr::string text(c.arena());
        text.append("proxy=").append(connection.viaTrustedProxy() ? "trusted" : "none");
        // scheme() describes the client's connection, including an explicitly
        // trusted proxy. tls() describes only the transport to this server.
        text.append(" scheme=").append(connection.scheme() == ruvia::HttpScheme::kHttps ? "https" : "http");
        text.append(" local_tls=").append(connection.tls() ? "yes" : "no");
        text.push_back('\n');
        co_return c.text(std::move(text));
    }

    ruvia::Task<void> fail_after_head(ruvia::Context& c) {
        co_await c.streamText().write("the response has already started\n");
        throw std::runtime_error("example streaming producer failed");
    }

    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return ruvia::makeHealthResponse(c);
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& c) {
        const bool databaseReady = c.req().query("db").value_or("up") != "down";
        co_return ruvia::makeReadinessResponse(
            c, {
                   .state = databaseReady ? ruvia::ReadinessState::kReady
                                          : ruvia::ReadinessState::kUnavailable,
                   .unavailableReason = "database is not ready",
               });
    }
};

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    const example::environment env(&app.env());
    if (env.get<bool>("RUVIA_TRUST_LOCAL_PROXY").value_or(false)) {
        app.trustedProxies({.cidrs = {"127.0.0.1", "::1"}, .trust_x_forwarded_proto = true});
    }
    app
        .listen({.address = "0.0.0.0", .http = 8080})
        .server({.worker_count = 2,
            .process_signal_handlers = ruvia::process_signal_handler_policy::install})
        // Each worker counts independently. Route-level limits additionally
        // apply and can only tighten the deployment-wide rule.
        .rateLimit({.rule = {.maxRequests = 100, .window = std::chrono::seconds(1)}})
        .onStart([] {
            // Runs on the thread calling run(), after worker capabilities are
            // ready and before any listener starts serving. Throwing rolls back
            // startup; wait for any posted initialization before returning.
            std::puts("all workers ready; opening admission");
        })
        .onStop([] {
            // Stop external producers here. Worker teardown is joined after
            // this hook; other threads should only request App::stop().
            std::puts("admission closed; joining workers");
        })
        .onAccess([](const ruvia::AccessLogRecord& record) noexcept {
            // Views are callback-scoped. Production sinks should enqueue an
            // owned record to a bounded logger instead of blocking the worker.
            std::printf("%.*s %.*s in %llu us\n",
                static_cast<int>(record.method().size()), record.method().data(),
                static_cast<int>(record.path().size()), record.path().data(),
                static_cast<unsigned long long>(record.durationMicros()));
        })
        .onConnectionFailure([](const ruvia::ConnectionFailureRecord& record) noexcept {
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
