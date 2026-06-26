#include <string_view>

#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"
#include "ruvia/http/Health.h"
#include "ruvia/http/SecurityHeaders.h"

class OpsController final : public ruvia::Controller<OpsController> {
public:
    RUVIA_CONTROLLER_GROUP("/admin", ruvia::SecurityHeadersMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    RUVIA_GET("/ready", ready);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return ruvia::makeHealthResponse(c);
    }

    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& c) {
        const bool databaseReady = c.query("db").toStringView().value_or("up") != "down";
        co_return ruvia::makeReadyResponse(c, databaseReady, "database is not ready");
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8080)
        .setThreadNum(2)
        .run();
}
