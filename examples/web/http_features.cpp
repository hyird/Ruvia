// HTTP/1 server features through the Web API.
// Run ruvia_example_http_features, then ruvia_example_http_client_advanced.
// Plain HTTP listens on 127.0.0.1:8093. Set RUVIA_TLS_CERT and RUVIA_TLS_KEY
// to enable HTTPS on port 8444; clients trust the local CA through RUVIA_TLS_CA.

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

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
    RUVIA_GET_STREAM("/download", download);
    RUVIA_POST_STREAM("/upload", upload, ruvia::body_limit<1024 * 1024>);
    RUVIA_GET_WS("/ws", websocket);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> info(ruvia::context& c) {
        const std::array<ruvia::http_header_view, 1> hints{{
            {"link", "</features/asset>; rel=preload; as=style"},
        }};
        // An interim head is separate from the final response. The Web API
        // copies its input, but the operation must still finish in this request.
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, hints));
        co_return c.text("interim responses\n");
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
