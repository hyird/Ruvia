// Outbound HTTP client usage from a Ruvia Controller.
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

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Dotenv.h"
#include "ruvia/web/HttpClientHandle.h"

#include "environment.h"

class GatewayController final : public ruvia::Controller<GatewayController> {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/forward", forward);
    RUVIA_GET_STREAM("/forward-stream", forwardStream);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> forward(ruvia::Context& c) {
        const auto incomingBody = co_await c.req().text();
        auto client = c.httpClient();

        try {
            std::array<ruvia::HttpHeaderView, 2> headers{};
            std::size_t headerCount = 0;
            headers[headerCount++] = {
                "content-type",
                c.req().header("content-type").value_or("application/octet-stream"),
            };
            if (const auto authorization = c.req().header("authorization")) {
                headers[headerCount++] = {"authorization", *authorization};
            }
            auto operation =
                client
                    .withOptions({
                        .timeout = std::chrono::seconds(5),
                    })
                    .send({
                        .method = "POST",
                        .target = "/features/upload",
                        .headers = std::span(headers).first(headerCount),
                        .content = ruvia::HttpClientRequestContentView::bytes(incomingBody),
                    });
            auto response = co_await std::move(operation);
            c.status(response.status());
            if (const auto contentType = response.header("content-type")) {
                c.header("content-type", *contentType);
            }
            // readAll() also reserves against this client pool's retained-result budget.
            auto body = co_await response.body().readAll();
            co_return c.body(body.bytes());
        } catch (const ruvia::HttpClientError& error) {
            const auto status = error.code() == ruvia::HttpClientError::Code::kTimeout
                                    ? ruvia::http_status::kGatewayTimeout
                                    : ruvia::http_status::kBadGateway;
            co_return c.error(
                {.status = status, .code = "upstream_error", .message = error.what()});
        }
    }

    ruvia::Task<void> forwardStream(ruvia::Context& c) {
        auto client = c.httpClient();
        std::array<ruvia::HttpHeaderView, 1> headers{};
        std::size_t headerCount = 0;
        if (const auto authorization = c.req().header("authorization")) {
            headers[headerCount++] = {"authorization", *authorization};
        }
        std::string errorBody;
        try {
            auto response = co_await client.send({
                .target = "/features/download",
                .headers = std::span(headers).first(headerCount),
            });
            c.status(response.status());
            if (const auto contentType = response.header("content-type")) {
                c.header("content-type", *contentType);
            }
            co_await response.body().pipeTo(c.stream());
        } catch (const ruvia::HttpClientError& error) {
            c.status(error.code() == ruvia::HttpClientError::Code::kTimeout
                         ? ruvia::http_status::kGatewayTimeout
                         : ruvia::http_status::kBadGateway);
            errorBody = error.what();
        }
        if (!errorBody.empty()) {
            co_await c.streamText().write(errorBody);
        }
    }
};

int main() {
    const example::environment env;
    const bool tls = env.get<bool>("RUVIA_UPSTREAM_TLS").value_or(false);
    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8080})
        .server({.worker_count = 2, .process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .httpClient({
            .config =
                {
                    .scheme = tls ? ruvia::HttpScheme::kHttps : ruvia::HttpScheme::kHttp,
                    .host = std::string(env.get("RUVIA_UPSTREAM_HOST").value_or("localhost")),
                    .port = env.get<std::uint16_t>("RUVIA_UPSTREAM_PORT").value_or(tls ? 8444 : 8093),
                    .connectionCount = 4,
                    .protocol = ruvia::HttpClientProtocol::kNegotiate,
                    .receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend,
                    .caFile = std::string(env.get("RUVIA_TLS_CA").value_or("")),
                },
        })
        .run();
}
