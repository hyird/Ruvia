// Middleware Next: the value Next and one-shot co_await next() signatures.

#include "ruvia/web/App.h"
#include "ruvia/web/Controller.h"

#include <type_traits>
#include <utility>

template <typename T>
concept HasStorableNextAwaiter = requires(T& next) {
    requires std::is_move_constructible_v<decltype(next().operator co_await())>;
};

static_assert(!std::is_copy_constructible_v<ruvia::Next>);
static_assert(!std::is_copy_assignable_v<ruvia::Next>);
static_assert(!std::is_move_constructible_v<ruvia::Next>);
static_assert(!std::is_move_assignable_v<ruvia::Next>);
static_assert(!std::is_copy_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_assignable_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_assignable_v<ruvia::Next::Awaitable>);
static_assert(!HasStorableNextAwaiter<ruvia::Next>);

class ValueNextMiddleware final : public ruvia::Middleware<ValueNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
    }
};

class ReusedNextAwaitableMiddleware final : public ruvia::Middleware<ReusedNextAwaitableMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        auto downstream = next();
        co_await std::move(downstream);
        co_await std::move(downstream);
    }
};

class MiddlewareNextController final : public ruvia::Controller<MiddlewareNextController> {
public:
    RUVIA_CONTROLLER_GROUP("/middleware-next", ValueNextMiddleware)

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", ok);
    RUVIA_GET("/reused-awaitable", ok, ReusedNextAwaitableMiddleware);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> ok(ruvia::Context& c) {
        co_return c.text("ok\n");
    }
};

int main() {
    ruvia::app()
        .setListenAddress("0.0.0.0")
        .setServerTopology(ruvia::ServerTopology::http(8089))
        .setWorkersPerListener(1)
        .run();
}
