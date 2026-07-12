#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/core/detail/AsioAwait.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <memory_resource>

namespace {

class ByRefNextMiddleware final : public ruvia::Middleware<ByRefNextMiddleware> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&);
};

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

static_assert(ruvia::detail::VoidHandleMiddleware<ByRefNextMiddleware>);
static_assert(!ruvia::detail::VoidHandleMiddleware<ByValueNextMiddleware>);
static_assert(!ruvia::detail::VoidHandleMiddleware<ConstNextMiddleware>);
static_assert(!ruvia::detail::VoidHandleMiddleware<RvalueNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<ByValueNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<ConstNextMiddleware>);
static_assert(!ruvia::detail::ResponseHandleMiddleware<RvalueNextMiddleware>);

int continuationCalls = 0;

ruvia::Task<void> countingContinuation(ruvia::Next::State) {
    ++continuationCalls;
    co_return;
}

ruvia::Task<int> exerciseExpiredNext() {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::Next::State::Control control;
    auto next = ruvia::detail::NextAccess::make(
        ruvia::Next::State{.control = &control},
        &countingContinuation);

    control.active = false;
    co_await next();

    co_return continuationCalls == 0 ? 0 : 1;
}

asio::awaitable<void> runExercise(int& result) {
    result = co_await ruvia::detail::taskAsAwaitable(exerciseExpiredNext());
}

}  // namespace

int main() {
    asio::io_context io;
    int result = 2;
    asio::co_spawn(io, runExercise(result), asio::detached);
    io.run();
    return result;
}
