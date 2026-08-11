// Outbound HTTP client usage from a Ruvia Controller.

#include <chrono>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/HttpClientHandle.h"

class GatewayController final : public ruvia::Controller<GatewayController> {
public:
    RUVIA_CONTROLLER_GROUP("/api")

    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/forward", forward);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> forward(ruvia::Context& c) {
        const auto incomingBody = co_await c.req().text();
        auto client = c.httpClient("backend");
        auto request = client.newRequest();
        request.setMethod(ruvia::HttpKnownMethod::kPost)
            .setTarget("/v1/orders")
            .setContentType(c.req().header("content-type").value_or("application/octet-stream"))
            .setBody(incomingBody);
        if (const auto authorization = c.req().header("authorization")) {
            request.addHeader("authorization", *authorization);
        }

        try {
            auto operation = client.sendRequest(std::move(request), {
                .timeout = std::chrono::seconds(5),
            });
            auto response = co_await std::move(operation);
            c.status(response.status());
            if (const auto contentType = response.header("content-type")) {
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
    ruvia::HttpClientConfig backend;
    backend.host = "api.example.com";
    backend.scheme = ruvia::HttpScheme::kHttps;
    backend.protocol = ruvia::HttpClientProtocol::kNegotiate;
    backend.connectionsPerWorker = 4;
    backend.cookiesEnabled = true;
    ruvia::app()
        .setListeners({ruvia::ListenerConfig::http("0.0.0.0", 8080)})
        .useHttpClient("backend", std::move(backend))
        .run();
}
