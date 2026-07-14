#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <memory>

namespace {

ruvia::Task<void> markAfterSleep(ruvia::WorkerHandle worker, bool& completed) {
    co_await ruvia::sleepFor(worker, std::chrono::hours(1));
    completed = true;
}

ruvia::Task<void> exercise(
    const std::shared_ptr<ruvia::detail::WorkerDispatcher>& dispatcher,
    ruvia::WorkerHandle worker,
    bool& success) {
    co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));

    bool cancelledSleepResumed = false;
    ruvia::TaskScope scope(worker);
    scope.spawn(markAfterSleep(worker, cancelledSleepResumed));
    dispatcher->stopTimers();
    co_await scope.join();
    success = cancelledSleepResumed;
}

}

int main() {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    asio::co_spawn(ioContext,
                   ruvia::detail::taskAsAwaitable(exercise(dispatcher, worker, success)),
                   asio::detached);
    ioContext.run();
    dispatcher->close();
    return success ? 0 : 1;
}
