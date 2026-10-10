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

#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"

#include "environment.h"

namespace {

class replay_safe final : public ruvia::middleware {
public:
    // This asserts that the ENTIRE route chain can safely run again. Restrict
    // it to read-only handlers; a GET name alone does not guarantee safety.
    static constexpr bool ruvia_replay_safe = true;
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

class feature_controller final : public ruvia::controller<feature_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/features")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/info", info);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/safe", safe, replay_safe);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET_STREAM("/download", download);
    RUVIA_POST_STREAM("/upload", upload, ruvia::body_limit<1024 * 1024>);
    RUVIA_GET_WS("/ws", websocket);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> info(ruvia::context& c) {
        const auto connection = c.conn();
        const std::array<ruvia::http_header_view, 1> hints{{
            {"link", "</features/asset>; rel=preload; as=style"},
        }};
        // An interim head is separate from the final response. The Web API
        // copies its input, but the operation must still finish in this request.
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, hints));
        c.priority({.urgency_ = 2, .incremental_ = true});
        if (connection.tls() && c.req().protocol_version() == ruvia::http_protocol_version::http2) {
            // ORIGIN and ALTSVC frames are HTTP/2 connection advertisements.
            // Observing an advertisement never changes a client's trusted origin.
            const std::array<std::string_view, 1> origins{"https://localhost:8444"};
            co_await c.advertise_origins(origins);
            if (connection.tls()) {
                co_await c.advertise_alternative_service("h3=\":8444\"; ma=60");
            }
        }
        if (c.req().protocol_version() == ruvia::http_protocol_version::http2 ||
            c.req().protocol_version() == ruvia::http_protocol_version::http3) {
            // Push can be declined by peer settings or capacity. Ordinary asset
            // requests remain available regardless of whether the promise wins.
            const bool pushed = co_await c.push({
                .scheme_ = connection.tls() ? "https" : "http",
                .authority_ = c.req().header("host").value_or("localhost:8444"),
                .path_ = "/features/asset",
            });
            c.header("X-Push-Accepted", pushed ? "yes" : "no");
        }
        co_return c.text("interim responses, priorities, advertisements, and push\n");
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        c.header("content-type", "text/css");
        co_return c.body(ruvia::static_text("body { color: #234; }\n"));
    }

    ruvia::task<ruvia::http_response> safe(ruvia::context& c) {
        const auto provenance = c.early_data_info();
        // The local transport flag is authoritative. Early-Data from an
        // upstream proxy is separately recorded and cannot forge that flag.
        c.header("X-Local-Early-Data", provenance.received_from_early_data() ? "1" : "0");
        c.header("X-Upstream-Early-Data", provenance.upstream_declared_early_data() ? "1" : "0");
        co_return c.text("read-only resource\n");
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        co_return c.redirect({.location_ = "/features/safe", .status_ = ruvia::http_status::see_other});
    }

    ruvia::task<void> download(ruvia::context& c) {
        auto& output = c.stream_text();
        c.header("Trailer", "X-Example-Complete");
        co_await output.write("first\n");
        // Await each write for bounded backpressure. No subsequent header or
        // cookie mutation is allowed once the first write publishes the head.
        co_await output.write("second\n");
        const std::array<ruvia::http_header_view, 1> trailers{{{"X-Example-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& body = c.req().get_body_reader();
        while (const auto chunk = co_await body.read()) {
            bytes_value += chunk->size();
        }
        // Request trailers are available only after the request body reaches EOF.
        std::pmr::string result_value(c.arena());
        result_value.append("uploaded=").append(std::to_string(bytes_value));
        result_value.append(" trailers=").append(std::to_string(c.req().trailers().size()));
        result_value.push_back('\n');
        co_return c.text(std::move(result_value));
    }

    ruvia::task<void> websocket(ruvia::context& c) {
        auto& socket = c.get_websocket();
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
    app.load_dotenv();
    const example::environment env_value(&app.env());
    ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8093};
    const auto certificate = env_value.get("RUVIA_TLS_CERT");
    const auto key = env_value.get("RUVIA_TLS_KEY");
    if (certificate.has_value() != key.has_value()) {
        throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
    }
    if (certificate && key) {
        listener_value.https_ = 8444;
        listener_value.tls_.certificate_chain_file_ = std::string(*certificate);
        listener_value.tls_.private_key_file_ = std::string(*key);
        listener_value.tls_.http3_early_data_ = env_value.get<bool>("RUVIA_EARLY_DATA").value_or(false);
        // Client path migration is enabled by default. Changing a client's
        // address or UDP port keeps the connection on its original worker.
        listener_value.http3_ = {.mode_ = ruvia::http3_mode::enabled,
            .qpack_ = {.max_table_capacity_ = 4096, .max_blocked_streams_ = 16}};
        listener_value.alt_svc_.max_age_ = std::chrono::seconds(60);
    }
    app.server({.worker_count_ = 2,
                   .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .listen(std::move(listener_value))
        .compression({.min_bytes_ = 16})
        .run();
}
