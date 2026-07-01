#include "ruvia/app/App.h"
#include "ruvia/http/Controller.h"

#include <type_traits>
#include <utility>

template <typename T>
concept HasStorableNextAwaiter = requires(T& next) {
    requires std::is_move_constructible_v<decltype(next().operator co_await())>;
};

static_assert(!std::is_move_constructible_v<ruvia::Next>);
static_assert(!std::is_move_assignable_v<ruvia::Next>);
static_assert(!std::is_copy_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_copy_assignable_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_constructible_v<ruvia::Next::Awaitable>);
static_assert(!std::is_move_assignable_v<ruvia::Next::Awaitable>);
static_assert(!HasStorableNextAwaiter<ruvia::Next>);

class ByValueNextMiddleware final : public ruvia::Middleware<ByValueNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next);
};

class ConstNextMiddleware final : public ruvia::Middleware<ConstNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, const ruvia::Next&);
};

class RvalueNextMiddleware final : public ruvia::Middleware<RvalueNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&&);
};

static_assert(!ruvia::detail::VoidHandleMiddleware<ByValueNextMiddleware>);
static_assert(!ruvia::detail::VoidHandleMiddleware<ConstNextMiddleware>);
static_assert(!ruvia::detail::VoidHandleMiddleware<RvalueNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<ByValueNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<ConstNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<RvalueNextMiddleware>);

class BorrowedNextMiddleware final : public ruvia::Middleware<BorrowedNextMiddleware> {
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
    RUVIA_CONTROLLER_GROUP("/middleware-next", BorrowedNextMiddleware)

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
        .setListenAddress("0.0.0.0", 8089)
        .setThreadNum(1)
        .run();
}
