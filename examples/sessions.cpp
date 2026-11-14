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
#include <exception>
#include <iostream>

#include "app.h"
#include "backend_config.h"
#include "controller.h"
#include "middleware.h"

namespace {

class session_controller final {
public:
    // session_middleware loads Redis data before entering the route. CSRF uses
    // a separate readable cookie, while the session ID cookie is HttpOnly.
    RUVIA_CONTROLLER_GROUP("/session", ruvia::session_middleware, ruvia::csrf_protection)
    RUVIA_ROUTES_BEGIN(session_controller)
    RUVIA_GET("/", read);
    RUVIA_POST("/", write, ruvia::body_limit<1024>);
    RUVIA_POST("/rotate", rotate);
    RUVIA_POST("/clear", clear);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> read(ruvia::context& c) {
        const auto session_value = c.session();
        // A read leaves the existing ID unchanged. The handle and data view
        // borrow context and must never be retained by background work.
        co_return c.text(session_value.data().empty() ? "no preference\n" : session_value.data());
    }

    ruvia::task<ruvia::http_response> write(ruvia::context& c) {
        const auto preference = co_await c.req().text();
        c.session().set(preference);
        // The middleware commits before response publication; every non-empty
        // write also rotates the ID. Mutate before a stream's first write or
        // a websocket handshake, since changes after publication are rejected.
        co_return c.text("preference saved\n");
    }

    ruvia::task<ruvia::http_response> rotate(ruvia::context& c) {
        c.session().regenerate();
        co_return c.text("session rotated\n");
    }

    ruvia::task<ruvia::http_response> clear(ruvia::context& c) {
        c.session().clear();
        co_return c.text("session cleared\n");
    }
};

}  // namespace

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        app.server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install}).listen({.address_ = "127.0.0.1", .http_ = 8092}).redis({.config_ = example::redis_config(env_value)}).run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
