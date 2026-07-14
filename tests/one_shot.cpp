#include <ruvia/core/OneShot.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <memory>

namespace {

ruvia::Task<void> waitForCompletion(ruvia::OneShotReceiver<int>& receiver, bool& success) {
    const auto result = co_await receiver.waitFor(std::chrono::seconds(1));
    success = result.status == ruvia::OneShotWaitStatus::kValue && result.value == 42;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        if (completion.complete(7) != ruvia::OneShotCompleteResult::kCompleted ||
            completion.complete(8) != ruvia::OneShotCompleteResult::kAlreadyCompleted) {
            co_return;
        }
        const auto result = co_await receiver.wait();
        if (result.status != ruvia::OneShotWaitStatus::kValue || result.value != 7) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto timeout = co_await receiver.waitFor(std::chrono::milliseconds(1));
        if (timeout.status != ruvia::OneShotWaitStatus::kTimeout ||
            completion.complete(9) != ruvia::OneShotCompleteResult::kCompleted) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (late.status != ruvia::OneShotWaitStatus::kValue || late.value != 9) {
            co_return;
        }
    }

    auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
    ruvia::TaskScope scope(worker);
    scope.spawn(waitForCompletion(receiver, success));
    if (completion.complete(42) != ruvia::OneShotCompleteResult::kCompleted) {
        co_return;
    }
    co_await scope.join();
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
    dispatcher->stopTimers();
    return success ? 0 : 1;
}
