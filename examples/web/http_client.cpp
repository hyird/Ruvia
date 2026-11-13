// Outbound HTTP client usage from a Ruvia controller.
// First run ruvia_example_http_features (localhost:8093), then this gateway
// (localhost:8099). POST /api/forward or GET /api/forward-stream.
// RUVIA_UPSTREAM_HOST/PORT override the fixed upstream; RUVIA_UPSTREAM_TLS=true
// enables HTTPS and RUVIA_TLS_CA selects a custom CA file. Verification stays on.
// One configured pool per worker is reused across requests. The handle borrows
// that pool and must not escape its worker; retained bytes have their own owner.

#include "ruvia/web/http_client.h"

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/error.h"
#include "ruvia/web/streaming.h"

#include "environment.h"

namespace {

struct upstream_failure final {
    ruvia::http_status_code status_;
    std::string_view message_;
};

// Transport errors can name the upstream host, port, TLS details or OS text,
// so they stay in the server log and clients get a fixed message. Production
// code should enqueue the record to a bounded logger instead of blocking here.
upstream_failure report_upstream_failure(const ruvia::http_client_error& error) {
    std::cerr << "upstream request failed: " << error.what() << '\n';
    if (error.code() == ruvia::http_client_error::code_type::timeout) {
        return {.status_ = ruvia::http_status::gateway_timeout, .message_ = "upstream timeout"};
    }
    return {.status_ = ruvia::http_status::bad_gateway, .message_ = "upstream unavailable"};
}

}  // namespace

class gateway_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN(gateway_controller)
    RUVIA_POST("/forward", forward);
    RUVIA_GET("/forward-stream", forward_stream);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> forward(ruvia::context& c) {
        const auto incoming_body = co_await c.req().bytes();
        auto client = c.client();

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
                    .post("/features/upload")
                    .headers(std::span(headers).first(header_count))
                    .body(incoming_body)
                    .send();
            auto response = co_await std::move(operation);
            c.draft().status(response.status());
            if (const auto content_type = response.header("content-type")) {
                c.draft().header("content-type", *content_type);
            }
            // bytes() also reserves against this client pool's retained-result budget.
            auto body = co_await response.bytes();
            co_return c.body(body.bytes());
        } catch (const ruvia::http_client_error& error) {
            const auto failure = report_upstream_failure(error);
            co_return c.error(
                {.status_ = failure.status_, .code_ = "upstream_error", .message_ = failure.message_});
        }
    }

    ruvia::task<void> forward_stream(ruvia::context& c) {
        auto client = c.client();
        std::array<ruvia::http_header_view, 1> headers{};
        std::size_t header_count = 0;
        if (const auto authorization = c.req().header("authorization")) {
            headers[header_count++] = {"authorization", *authorization};
        }
        // Only a failure before the stream starts can still become an error
        // response. Once pipe_to has sent the head, an upstream failure must
        // propagate so the server aborts the stream instead of ending a 200
        // whose body is silently truncated.
        std::optional<ruvia::http_client_response> response;
        std::string_view error_body;
        try {
            response.emplace(co_await client.get("/features/download")
                    .headers(std::span(headers).first(header_count))
                    .send());
        } catch (const ruvia::http_client_error& error) {
            const auto failure = report_upstream_failure(error);
            c.draft().status(failure.status_);
            error_body = failure.message_;
        }
        if (!response) {
            if (co_await c.stream_text().commit() == ruvia::stream_commit_result::body) {
                co_await c.stream().write(error_body);
            }
            co_return;
        }
        co_await response->pipe_to(c);
    }
};

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env(&app.env());
        const bool tls = env.get<bool>("RUVIA_UPSTREAM_TLS").value_or(false);
        const std::string_view host = env.get("RUVIA_UPSTREAM_HOST").value_or("localhost");
        std::pmr::string base_url(tls ? "https://" : "http://", ruvia::process_resource());
        const bool ipv6 = host.find(':') != std::string_view::npos;
        if (ipv6) {
            base_url.push_back('[');
        }
        base_url.append(host);
        if (ipv6) {
            base_url.push_back(']');
        }
        base_url.push_back(':');
        ruvia::http_append_unsigned(base_url, env.get<std::uint16_t>("RUVIA_UPSTREAM_PORT").value_or(tls ? 8444 : 8093));
        app
            .listen({.address_ = "0.0.0.0", .http_ = 8099})
            .server({.worker_count_ = 2, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .client({
                .base_url_ = base_url,
                .connection_limits_ = {.max_open_ = 16, .max_idle_http1_ = 8, .max_idle_multiplexed_ = 2},
                .tls_ = {.ca_file_ = env.path("RUVIA_TLS_CA")},
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
