// Middleware continuation, response decoration, and early responses.
// Run ruvia_example_middleware_next on port 8089.
// curl -i http://127.0.0.1:8089/middleware-next
// /middleware-next/protected returns 401 unless X-Demo-Key: example is present.

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

class decorate_response final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        context.header("X-Before", "set");
        // next() is a single-shot continuation. Await it exactly once; do not
        // store it, detach it, or attempt to restart the downstream handler.
        co_await next();
        context.header("X-After", "set");
    }
};

class require_demo_key final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        if (context.req().header("X-Demo-Key") != "example") {
            // An early response skips this branch. Outer middleware still
            // resumes after its own awaited next().
            context.status(ruvia::http_status::kUnauthorized);
            context.respond(context.text("demo key required\n"));
            co_return;
        }
        co_await next();
    }
};

class middleware_next_controller final : public ruvia::Controller<middleware_next_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/middleware-next", decorate_response)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", ok);
    RUVIA_GET("/protected", ok, require_demo_key);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> ok(ruvia::Context& context) {
        co_return context.text("ok\n");
    }
};

int main() {
    ruvia::app()
        .listen({.address = "0.0.0.0", .http = 8089})
        .server({.worker_count = 1,
            .process_signal_handlers = ruvia::process_signal_handler_policy::install})
        .run();
}
