// Redis-backed sessions with browser CSRF protection.
// Requires RUVIA_ENABLE_REDIS=ON and Redis at RUVIA_REDIS_HOST/PORT (localhost:6379).
// Run ruvia_example_sessions, then GET http://127.0.0.1:8092/session.
// Keep the returned XSRF-TOKEN cookie and repeat it in X-XSRF-TOKEN for POSTs:
//   POST /session with a text body stores a demo preference, not an identity.
//   POST /session/rotate rotates its cookie; POST /session/clear removes it.
// Reuse the newest sid cookie on every request. Concurrent stale writes return
// 409; do not retry stale authenticated state under a newly created session.
// backend_tls.h defines RUVIA_REDIS_TLS/CA/CERT/KEY; set TLS=false explicitly
// for local plaintext Redis. Optional USER/PASSWORD/DATABASE select its account.

#include <chrono>

#include "ruvia/web/App.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Csrf.h"
#include "ruvia/web/Session.h"

#include "backend_tls.h"

namespace {

class session_controller final : public ruvia::Controller<session_controller> {
public:
    // SessionMiddleware loads Redis data before entering the route. CSRF uses
    // a separate readable cookie, while the session ID cookie is HttpOnly.
    RUVIA_CONTROLLER_GROUP("/session", ruvia::SessionMiddleware, ruvia::CsrfProtection)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", read);
    RUVIA_POST("/", write, ruvia::BodyLimit<1024>);
    RUVIA_POST("/rotate", rotate);
    RUVIA_POST("/clear", clear);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> read(ruvia::Context& c) {
        const auto session = c.session();
        // A read leaves the existing ID unchanged. The handle and data view
        // borrow Context and must never be retained by background work.
        co_return c.text(session.data().empty() ? "no preference\n" : session.data());
    }

    ruvia::Task<ruvia::HttpResponse> write(ruvia::Context& c) {
        const auto preference = co_await c.req().text();
        c.session().set(preference);
        // The middleware commits before response publication; every non-empty
        // write also rotates the ID. Mutate before a stream's first write or
        // a WebSocket handshake, since changes after publication are rejected.
        co_return c.text("preference saved\n");
    }

    ruvia::Task<ruvia::HttpResponse> rotate(ruvia::Context& c) {
        c.session().regenerate();
        co_return c.text("session rotated\n");
    }

    ruvia::Task<ruvia::HttpResponse> clear(ruvia::Context& c) {
        c.session().clear();
        co_return c.text("session cleared\n");
    }
};

}  // namespace

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();
    const example::environment env(&app.env());
    app.server({.process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .listen({.address = "127.0.0.1", .http = 8092})
        .redis({.config = {
                    .host = std::string(env.get("RUVIA_REDIS_HOST").value_or("127.0.0.1")),
                    .port = env.get<std::uint16_t>("RUVIA_REDIS_PORT").value_or(6379),
                    .username = std::string(env.get("RUVIA_REDIS_USER").value_or("")),
                    .password = std::string(env.get("RUVIA_REDIS_PASSWORD").value_or("")),
                    .tls = example::backend_tls("RUVIA_REDIS", env),
                    .database = env.get<std::uint32_t>("RUVIA_REDIS_DATABASE").value_or(0),
                }})
        .run();
}
