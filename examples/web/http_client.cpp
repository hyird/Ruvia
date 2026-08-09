// Drogon-style outbound HTTP client usage from a Ruvia Controller.

#include <chrono>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/HttpClient.h"

const auto backendClient = [] {
    ruvia::HttpClientConfig config;
    config.protocol = ruvia::HttpClientProtocol::kNegotiate;
    config.connectionsPerWorker = 4;
    config.cookiesEnabled = true;
    return ruvia::HttpClient::newHttpClient("https://api.example.com", std::move(config));
}();

class GatewayController final : public ruvia::Controller<GatewayController> {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/forward", forward);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> forward(ruvia::Context& c) {
        const auto incomingBody = co_await c.req().text();
        auto request = backendClient->newRequest();
        request.setMethod(ruvia::HttpKnownMethod::kPost)
            .setPath("/v1/orders")
            .setContentTypeString(c.req().header("content-type").value_or("application/octet-stream"))
            .setBody(incomingBody);
        if (const auto authorization = c.req().header("authorization")) {
            request.addHeader("authorization", *authorization);
        }

        try {
            auto operation = backendClient->sendRequest(std::move(request), {
                .timeout = std::chrono::seconds(5),
                .stopToken = c.stopToken(),
            });
            auto response = co_await std::move(operation);
            c.status(response.statusCode());
            if (const auto contentType = response.getHeader("content-type")) {
                c.header("content-type", *contentType);
            }
            co_return c.body(response.body());
        } catch (const ruvia::HttpClientError& error) {
            const auto status = error.code() == ruvia::HttpClientError::Code::kTimeout
                ? ruvia::http_status::kGatewayTimeout
                : ruvia::http_status::kBadGateway;
            co_return c.error(status, "upstream_error", error.what());
        }
    }
};

int main() {
    ruvia::app()
        .setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8080)})
        .run();
}
