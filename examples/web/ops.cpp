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

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    // Configuration travels in the type, so a route-level middleware needs no
    // constructor arguments and no hand-written wrapper class.
    RUVIA_GET("/ready", ready, ruvia::RateLimit<10, 1000>);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return ruvia::makeHealthResponse(c);
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& c) {
        const bool databaseReady = c.req().query("db").value_or("up") != "down";
        co_return ruvia::makeReadinessResponse(c, {
            .state = databaseReady ? ruvia::ReadinessState::kReady : ruvia::ReadinessState::kUnavailable,
            .unavailableReason = "database is not ready",
        });
    }
};

int main() {
    ruvia::app().setListeners({ruvia::ListenerConfig::http({.address = "0.0.0.0", .port = 8080})}).setWorkersPerListener(2).setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall).run();
}
