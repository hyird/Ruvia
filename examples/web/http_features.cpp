// HTTP/1, HTTP/2, and HTTP/3 server features through the Web API.
// Run ruvia_example_http_features, then run ruvia_example_http_client_advanced.
// Plain HTTP listens on 127.0.0.1:8093 and accepts HTTP/2 prior knowledge too.
// Set RUVIA_TLS_CERT and RUVIA_TLS_KEY to enable HTTPS and QUIC on port 8444.
// The certificate must cover localhost; clients use RUVIA_TLS_CA to trust it.
// Optional RUVIA_EARLY_DATA=true enables explicitly declared replay-safe routes.
// Try curl -i http://127.0.0.1:8093/features/info or /features/redirect.

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

#include "ruvia/web/App.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Controller.h"

#include "environment.h"

namespace {

class replay_safe final : public ruvia::Middleware {
public:
    // This asserts that the ENTIRE route chain can safely run again. Restrict
    // it to read-only handlers; a GET name alone does not guarantee safety.
    static constexpr bool ruvia_replay_safe = true;
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class feature_controller final : public ruvia::Controller<feature_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/features")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/info", info);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/safe", safe, replay_safe);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET_STREAM("/download", download);
    RUVIA_POST_STREAM("/upload", upload, ruvia::BodyLimit<1024 * 1024>);
    RUVIA_GET_WS("/ws", websocket);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> info(ruvia::Context& c) {
        const auto connection = c.conn();
        const std::array<ruvia::HttpHeaderView, 1> hints{{
            {"link", "</features/asset>; rel=preload; as=style"},
        }};
        // An interim head is separate from the final response. The Web API
        // copies its input, but the operation must still finish in this request.
        co_await c.inform(ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, hints));
        c.priority({.urgency = 2, .incremental = true});
        if (connection.tls() && c.req().protocolVersion() == ruvia::HttpProtocolVersion::kHttp2) {
            // ORIGIN and ALTSVC frames are HTTP/2 connection advertisements.
            // Observing an advertisement never changes a client's trusted origin.
            const std::array<std::string_view, 1> origins{"https://localhost:8444"};
            co_await c.advertiseOrigins(origins);
            if (connection.tls()) {
                co_await c.advertiseAlternativeService("h3=\":8444\"; ma=60");
            }
        }
        if (c.req().protocolVersion() == ruvia::HttpProtocolVersion::kHttp2 ||
            c.req().protocolVersion() == ruvia::HttpProtocolVersion::kHttp3) {
            // Push can be declined by peer settings or capacity. Ordinary asset
            // requests remain available regardless of whether the promise wins.
            const bool pushed = co_await c.push({
                .scheme = connection.tls() ? "https" : "http",
                .authority = c.req().header("host").value_or("localhost:8444"),
                .path = "/features/asset",
            });
            c.header("X-Push-Accepted", pushed ? "yes" : "no");
        }
        co_return c.text("interim responses, priorities, advertisements, and push\n");
    }

    ruvia::Task<ruvia::HttpResponse> asset(ruvia::Context& c) {
        c.header("content-type", "text/css");
        co_return c.body("body { color: #234; }\n");
    }

    ruvia::Task<ruvia::HttpResponse> safe(ruvia::Context& c) {
        const auto provenance = c.early_data_info();
        // The local transport flag is authoritative. Early-Data from an
        // upstream proxy is separately recorded and cannot forge that flag.
        c.header("X-Local-Early-Data", provenance.received_from_early_data() ? "1" : "0");
        c.header("X-Upstream-Early-Data", provenance.upstream_declared_early_data() ? "1" : "0");
        co_return c.text("read-only resource\n");
    }

    ruvia::Task<ruvia::HttpResponse> redirect(ruvia::Context& c) {
        co_return c.redirect({.location = "/features/safe", .status = ruvia::http_status::kSeeOther});
    }

    ruvia::Task<void> download(ruvia::Context& c) {
        auto& output = c.streamText();
        c.header("Trailer", "X-Example-Complete");
        co_await output.write("first\n");
        // Await each write for bounded backpressure. No subsequent header or
        // cookie mutation is allowed once the first write publishes the head.
        co_await output.write("second\n");
        const std::array<ruvia::HttpHeaderView, 1> trailers{{{"X-Example-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::Task<ruvia::HttpResponse> upload(ruvia::Context& c) {
        std::size_t bytes = 0;
        auto& body = c.req().bodyReader();
        while (const auto chunk = co_await body.read()) {
            bytes += chunk->size();
        }
        // Request trailers are available only after the request body reaches EOF.
        std::pmr::string result(c.arena());
        result.append("uploaded=").append(std::to_string(bytes));
        result.append(" trailers=").append(std::to_string(c.req().trailers().size()));
        result.push_back('\n');
        co_return c.text(std::move(result));
    }

    ruvia::Task<void> websocket(ruvia::Context& c) {
        auto& socket = c.webSocket();
        while (auto message = co_await socket.read()) {
            // Consume the borrowed payload before the next read replaces it.
            if (message->text()) {
                co_await socket.text(message->payload());
            } else if (message->binary()) {
                co_await socket.binary(message->payload());
            }
        }
    }
};

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    const example::environment env(&app.env());
    ruvia::ListenConfig listener{.address = "127.0.0.1", .http = 8093};
    const auto certificate = env.get("RUVIA_TLS_CERT");
    const auto key = env.get("RUVIA_TLS_KEY");
    if (certificate.has_value() != key.has_value()) {
        throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
    }
    if (certificate && key) {
        listener.https = 8444;
        listener.tls.certificateChainFile = std::string(*certificate);
        listener.tls.privateKeyFile = std::string(*key);
        listener.tls.http3_early_data = env.get<bool>("RUVIA_EARLY_DATA").value_or(false);
        // Client path migration is enabled by default. Changing a client's
        // address or UDP port keeps the connection on its original worker.
        listener.http3 = {.mode = ruvia::Http3Mode::kEnabled,
            .qpack = {.maxTableCapacity = 4096, .maxBlockedStreams = 16}};
        listener.altSvc.maxAge = std::chrono::seconds(60);
    }
    app.server({.worker_count = 2,
                   .process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .listen(std::move(listener))
        .compression({.minBytes = 16})
        .run();
}
