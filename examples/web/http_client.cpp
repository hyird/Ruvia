// Outbound HTTP client usage from a Ruvia Controller.

#include <array>
#include <chrono>
#include <span>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/HttpClientHandle.h"

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
                        .target = "/v1/orders",
                        .headers = std::span(headers).first(headerCount),
                        .content = ruvia::HttpClientRequestContentView::bytes(incomingBody),
                    });
            auto response = co_await std::move(operation);
            c.status(response.status());
            if (const auto contentType = response.header("content-type")) {
                c.header("content-type", *contentType);
            }
            co_return c.body(co_await response.body().readAll());
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
                .target = "/v1/events",
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
        if (!errorBody.empty()) co_await c.streamText().write(errorBody);
    }
};

int main() {
    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8080})
        .httpClient({
            .config =
                {
                    .scheme = ruvia::HttpScheme::kHttps,
                    .host = "api.example.com",
                    .connectionCount = 4,
                    .protocol = ruvia::HttpClientProtocol::kNegotiate,
                    .receivedCookies = ruvia::HttpClientReceivedCookiePolicy::kRetainAndSend,
                },
        })
        .run();
}
