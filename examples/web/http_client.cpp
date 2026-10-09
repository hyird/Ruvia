// Outbound HTTP client usage from a Ruvia controller.
// First run ruvia_example_http_features (localhost:8093), then this gateway
// (localhost:8080). POST /api/forward or GET /api/forward-stream.
// RUVIA_UPSTREAM_HOST/PORT override the fixed upstream; RUVIA_UPSTREAM_TLS=true
// enables HTTPS and RUVIA_TLS_CA selects a custom CA file. Verification stays on.
// One configured pool per worker is reused across requests. The handle borrows
// that pool and must not escape its worker; retained bytes have their own owner.

#include <array>
#include <chrono>
#include <span>
#include <string>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/http_client_handle.h"

#include "environment.h"

class gateway_controller final : public ruvia::controller<gateway_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/forward", forward);
    RUVIA_GET_STREAM("/forward-stream", forward_stream);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> forward(ruvia::context& c) {
        const auto incoming_body = co_await c.req().text();
        auto client = c.get_http_client();

        try {
            std::array<ruvia::http_header_view, 2> headers{};
            std::size_t header_count = 0;
            headers[header_count++] = {
                "content-type",
                c.req().header("content-type").value_or("application/octet-stream"),
            };
            if (const auto authorization = c.req().header("authorization")) {
                headers[header_count++] = {"authorization", *authorization};
            }
            auto operation =
                client
                    .with_options({
                        .timeout_ = std::chrono::seconds(5),
                    })
                    .send({
                        .method_ = "POST",
                        .target_ = "/features/upload",
                        .headers_ = std::span(headers).first(header_count),
                        .content_ = ruvia::http_client_request_content_view::bytes(incoming_body),
                    });
            auto response = co_await std::move(operation);
            c.status(response.status());
            if (const auto content_type = response.header("content-type")) {
                c.header("content-type", *content_type);
            }
            // read_all() also reserves against this client pool's retained-result budget.
            auto body = co_await response.body().read_all();
            co_return c.body(body.bytes());
        } catch (const ruvia::http_client_error& error) {
            const auto status = error.code() == ruvia::http_client_error::code_type::timeout
                                    ? ruvia::http_status::gateway_timeout
                                    : ruvia::http_status::bad_gateway;
            co_return c.error(
                {.status_ = status, .code_ = "upstream_error", .message_ = error.what()});
        }
    }

    ruvia::task<void> forward_stream(ruvia::context& c) {
        auto client = c.get_http_client();
        std::array<ruvia::http_header_view, 1> headers{};
        std::size_t header_count = 0;
        if (const auto authorization = c.req().header("authorization")) {
            headers[header_count++] = {"authorization", *authorization};
        }
        std::string error_body;
        try {
            auto response = co_await client.send({
                .target_ = "/features/download",
                .headers_ = std::span(headers).first(header_count),
            });
            c.status(response.status());
            if (const auto content_type = response.header("content-type")) {
                c.header("content-type", *content_type);
            }
            co_await response.body().pipe_to(c.stream());
        } catch (const ruvia::http_client_error& error) {
            c.status(error.code() == ruvia::http_client_error::code_type::timeout
                         ? ruvia::http_status::gateway_timeout
                         : ruvia::http_status::bad_gateway);
            error_body = error.what();
        }
        if (!error_body.empty()) {
            co_await c.stream_text().write(error_body);
        }
    }
};

int main() {
    const example::environment env;
    const bool tls = env.get<bool>("RUVIA_UPSTREAM_TLS").value_or(false);
    ruvia::app()
        .listen({.address_ = "0.0.0.0", .http_ = 8080})
        .server({.worker_count_ = 2, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .get_http_client({
            .config_ =
                {
                    .scheme_ = tls ? ruvia::http_scheme::https : ruvia::http_scheme::http,
                    .host_ = std::string(env.get("RUVIA_UPSTREAM_HOST").value_or("localhost")),
                    .port_ = env.get<std::uint16_t>("RUVIA_UPSTREAM_PORT").value_or(tls ? 8444 : 8093),
                    .connection_count_ = 4,
                    .protocol_ = ruvia::http_client_protocol::negotiate,
                    .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                    .ca_file_ = std::string(env.get("RUVIA_TLS_CA").value_or("")),
                },
        })
        .run();
}
