#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <memory>
#include <chrono>
#include <stdexcept>
#include <string_view>

namespace {

ruvia::Task<void> increment(ruvia::WorkerHandle worker, int& value) {
    co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));
    ++value;
    co_return;
}

ruvia::Task<void> fail() {
    throw std::runtime_error("child failed");
    co_return;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    int calls = 0;
    ruvia::TaskScope scope(worker);
    scope.spawn(increment(worker, calls));
    scope.spawn(fail());
    if (scope.size() != 2) {
        co_return;
    }

    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stopRequested() &&
                  std::string_view(error.what()) == "child failed";
    }
}

}

int main() {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;

    asio::co_spawn(ioContext,
                   ruvia::detail::taskAsAwaitable(exercise(worker, success)),
                   asio::detached);
    ioContext.run();
    dispatcher->close();
    return success ? 0 : 1;
}
