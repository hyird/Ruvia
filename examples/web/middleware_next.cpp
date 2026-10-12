// Middleware continuation, response decoration, and early responses.
// Run ruvia_example_middleware_next on port 8089.
// curl -i http://127.0.0.1:8089/middleware-next
// /middleware-next/protected returns 401 unless X-Demo-Key: example is present.

#include <exception>
#include <iostream>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"

class decorate_response final {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        context_value.header("X-Before", "set");
        // next() is a single-shot continuation. Await it exactly once; do not
        // store it, detach it, or attempt to restart the downstream handler.
        co_await next_value();
        context_value.header("X-After", "set");
    }
};

class require_demo_key final {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Demo-Key") != "example") {
            // An early response skips this branch. Outer middleware still
            // resumes after its own awaited next(). A 401 must carry a
            // challenge (RFC 9110 15.5.2); this demo's credential is the
            // X-Demo-Key header, so it names its own scheme rather than
            // Bearer, which would tell clients to send Authorization.
            context_value.status(ruvia::http_status::unauthorized);
            context_value.header("WWW-Authenticate", R"(DemoKey realm="middleware-next")");
            context_value.respond(context_value.text("demo key required\n"));
            co_return;
        }
        co_await next_value();
    }
};

class middleware_next_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/middleware-next", decorate_response)

    RUVIA_ROUTES_BEGIN(middleware_next_controller)
    RUVIA_GET("/", ok);
    RUVIA_GET("/protected", ok, require_demo_key);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> ok(ruvia::context& context) {
        co_return context.text("ok\n");
    }
};

int main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8089})
            .server({.worker_count_ = 1,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}
