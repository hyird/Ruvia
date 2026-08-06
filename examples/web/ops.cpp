// Operational middleware: security headers, route-level per-IP rate limiting,
// and health/readiness response helpers wired through controller macros.

#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Health.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/SecurityHeaders.h"

class OpsController final : public ruvia::Controller<OpsController> {
public:
    RUVIA_CONTROLLER_GROUP("/admin", ruvia::SecurityHeadersMiddleware)
    RUVIA_ROUTE_RATE_LIMIT(ReadyRateLimit, 10, 1000);

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    RUVIA_GET("/ready", ready, ReadyRateLimit);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return ruvia::makeHealthResponse(c);
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& c) {
        const bool databaseReady = c.req().query("db").value_or("up") != "down";
        co_return ruvia::makeReadyResponse(c, databaseReady, "database is not ready");
    }
};

int main() {
    ruvia::app().setListenAddress("0.0.0.0").setListeners({ruvia::ListenerConfig::http(8080)}).setWorkersPerListener(2).setSignalShutdown(true).run();
}
