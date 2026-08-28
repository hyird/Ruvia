#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

template <typename T>
concept HasRvalueTaskScopeSpawn = requires(T&& scope) { std::move(scope).spawn(ruvia::Task<void>{}); };

template <typename T>
concept HasRvalueTaskScopeRequestStop = requires(T&& scope) { std::move(scope).requestStop(); };

template <typename T>
concept HasRvalueTaskScopeStopToken = requires(const T&& scope) { std::move(scope).stopToken(); };

template <typename T>
concept HasRvalueTaskScopeStopRequested = requires(const T&& scope) { std::move(scope).stopRequested(); };

template <typename T>
concept HasRvalueTaskScopeSize = requires(const T&& scope) { std::move(scope).size(); };

template <typename T>
concept HasRvalueTaskScopeJoin = requires(T&& scope) { std::move(scope).join(); };

static_assert(std::is_move_constructible_v<ruvia::Task<void>>);
static_assert(!std::is_move_assignable_v<ruvia::Task<void>>);
static_assert(std::is_move_constructible_v<ruvia::Task<int>>);
static_assert(!std::is_move_assignable_v<ruvia::Task<int>>);
static_assert(std::is_aggregate_v<ruvia::TaskScopeOptions>);
static_assert(std::same_as<decltype(ruvia::TaskScopeOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::is_constructible_v<ruvia::TaskScope, const ruvia::WorkerHandle&>);
static_assert(std::is_constructible_v<ruvia::TaskScope, const ruvia::WorkerHandle&, ruvia::TaskScopeOptions>);
static_assert(!std::is_constructible_v<ruvia::TaskScope, const ruvia::WorkerHandle&, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::TaskScope, ruvia::WorkerHandle&&>);
static_assert(!std::is_constructible_v<ruvia::TaskScope, ruvia::WorkerHandle&&, ruvia::TaskScopeOptions>);
static_assert(!std::is_constructible_v<ruvia::TaskScope, ruvia::WorkerHandle&&, std::pmr::memory_resource*>);
static_assert(!HasRvalueTaskScopeSpawn<ruvia::TaskScope>);
static_assert(!HasRvalueTaskScopeRequestStop<ruvia::TaskScope>);
static_assert(!HasRvalueTaskScopeStopToken<ruvia::TaskScope>);
static_assert(!HasRvalueTaskScopeStopRequested<ruvia::TaskScope>);
static_assert(!HasRvalueTaskScopeSize<ruvia::TaskScope>);
static_assert(!HasRvalueTaskScopeJoin<ruvia::TaskScope>);

namespace {

ruvia::Task<void> increment(ruvia::WorkerHandle worker, int& value) {
    static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)));
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
        {
            auto discardedColdJoin = emptyScope.join();
            static_cast<void>(discardedColdJoin);
        }
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
        } catch (const std::logic_error&) {
            emptyTaskRejected = true;
        }
        if (!emptyTaskRejected || emptyTaskScope.size() != 0) {
            co_return;
        }
        co_await emptyTaskScope.join();
    }

    {
        ruvia::TaskScope reservedJoinScope(worker);
        reservedJoinScope.spawn(noOp());
        auto reservedJoin = reservedJoinScope.join();
        bool spawnAfterReservationRejected = false;
        try {
            reservedJoinScope.spawn(noOp());
        } catch (const std::logic_error&) {
            spawnAfterReservationRejected = true;
        }
        if (!spawnAfterReservationRejected) {
            co_return;
        }
        co_await std::move(reservedJoin);
    }

    {
        ruvia::TaskScope completedFailureScope(worker);
        completedFailureScope.spawn(fail());
        static_cast<void>(co_await ruvia::sleepFor(worker, std::chrono::milliseconds(1)));
        if (completedFailureScope.size() != 0) {
            co_return;
        }
        bool completedFailureObserved = false;
        try {
            co_await completedFailureScope.join();
        } catch (const std::runtime_error& error) {
            completedFailureObserved = std::string_view(error.what()) == "child failed";
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
        success = calls == 1 && scope.size() == 0 && scope.stopRequested() && std::string_view(error.what()) == "child failed";
    }
}

}  // namespace

int main() {
    ruvia::StopToken retainedToken;
    {
        ruvia::StopSource source;
        retainedToken = source.token();
        source.requestStop();
    }
    if (!retainedToken.stopRequested()) {
        return 1;
    }

    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;

    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise(worker, success)), asio::detached);
    ioContext.run();
    dispatcher->close();
    return success ? 0 : 1;
}
