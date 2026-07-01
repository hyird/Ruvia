#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

#include <type_traits>

static_assert(!std::is_move_constructible_v<ruvia::Next>);
static_assert(!std::is_move_assignable_v<ruvia::Next>);

class BorrowedNextMiddleware final : public ruvia::Middleware<BorrowedNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class MiddlewareNextController final : public ruvia::Controller<MiddlewareNextController> {
public:
    RUVIA_CONTROLLER_GROUP("/middleware-next", BorrowedNextMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", ok);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> ok(ruvia::Context& c) {
        co_return c.text("ok\n");
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0", 8089)
        .setThreadNum(1)
        .run();
}
