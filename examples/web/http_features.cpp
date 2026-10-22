// HTTP/1 and HTTP/2 server features through the Web API.
// Run ruvia_example_http_features, then ruvia_example_http_client_advanced [1|2].
// Plain HTTP listens on 127.0.0.1:8093 and accepts HTTP/2 prior knowledge.
// Set RUVIA_TLS_CERT and RUVIA_TLS_KEY to enable HTTPS on port 8444;
// the certificate must cover localhost and clients trust it through RUVIA_TLS_CA.

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/streaming.h"

#include "environment.h"

namespace {

class feature_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/features")
    RUVIA_ROUTES_BEGIN(feature_controller)
    RUVIA_GET("/info", info);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/safe", safe);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET("/download", download);
    RUVIA_POST("/upload", upload, ruvia::stream_body, ruvia::body_limit<1024 * 1024>);
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
        if (c.req().protocol_version() == ruvia::http_protocol_version::http2) {
            if (connection.tls()) {
                // Advertisements never change the client's trusted origin.
                const std::array<std::string_view, 1> origins{"https://localhost:8444"};
                co_await c.advertise_origins(origins);
                co_await c.advertise_alternative_service("h2=\":8444\"; ma=60");
            }
            // Peer settings or capacity can decline push; the asset remains
            // available through an ordinary request.
            const bool pushed = co_await c.push({
                .scheme_ = connection.tls() ? "https" : "http",
                .authority_ = c.req().header("host").value_or(connection.tls() ? "localhost:8444" : "localhost:8093"),
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
        auto& body = c.req().body_reader();
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
        auto& socket = c.websocket();
        while (auto message = co_await socket.read()) {
            // Borrow the payload for the echo; send() holds the read lane until
            // the frame is written, so the next read cannot replace it early.
            co_await socket.send(std::move(*message));
        }
    }
};

}  // namespace

int main() {
    try {
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
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen(std::move(listener_value))
            .compression({.min_bytes_ = 16})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
