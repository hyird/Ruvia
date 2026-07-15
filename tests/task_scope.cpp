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
#include <type_traits>
#include <utility>

static_assert(std::is_move_constructible_v<ruvia::Task<void>>);
static_assert(!std::is_move_assignable_v<ruvia::Task<void>>);
static_assert(std::is_move_constructible_v<ruvia::Task<int>>);
static_assert(!std::is_move_assignable_v<ruvia::Task<int>>);

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

ruvia::Task<void> noOp() {
    co_return;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    {
        ruvia::TaskScope emptyScope(worker);
        co_await emptyScope.join();
        bool secondJoinRejected = false;
        try {
            co_await emptyScope.join();
        } catch (const std::logic_error&) {
            secondJoinRejected = true;
        }
        bool spawnAfterJoinRejected = false;
        try {
            emptyScope.spawn(noOp());
        } catch (const std::logic_error&) {
            spawnAfterJoinRejected = true;
        }
        if (!secondJoinRejected || !spawnAfterJoinRejected) {
            co_return;
        }
    }

    {
        ruvia::TaskScope emptyTaskScope(worker);
        auto movedFrom = noOp();
        auto retained = std::move(movedFrom);
        static_cast<void>(retained);
        bool emptyTaskRejected = false;
        try {
            emptyTaskScope.spawn(std::move(movedFrom));
        } catch (const std::invalid_argument&) {
            emptyTaskRejected = true;
        }
        if (!emptyTaskRejected || emptyTaskScope.size() != 0) {
            co_return;
        }
        co_await emptyTaskScope.join();
    }

    {
        ruvia::TaskScope completedFailureScope(worker);
        completedFailureScope.spawn(fail());
        co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1));
        if (completedFailureScope.size() != 0) {
            co_return;
        }
        bool completedFailureObserved = false;
        try {
            co_await completedFailureScope.join();
        } catch (const std::runtime_error& error) {
            completedFailureObserved =
                std::string_view(error.what()) == "child failed";
        }
        if (!completedFailureObserved) {
            co_return;
        }
    }

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
