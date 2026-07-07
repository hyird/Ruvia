#include "router/RouteTable.h"
#include "runtime/AsioAwait.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <memory_resource>

namespace {

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
