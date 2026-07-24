#include <ruvia/core/BlockingPool.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

using ruvia::BlockingPool;
using ruvia::BlockingPoolOptions;
using ruvia::BlockingStatus;
using ruvia::BlockingSubmitStatus;
using ruvia::Task;
using ruvia::WorkerHandle;

// A task the pool runs on its own thread: it reports that it started, then
// holds its thread until the test releases it.
struct ThreadGate final {
    std::binary_semaphore started{0};
    std::binary_semaphore release{0};

    void occupy(BlockingPool& pool) {
        const auto submitted = pool.submit([this] {
            started.release();
            release.acquire();
        });
        if (submitted != BlockingSubmitStatus::kAccepted) {
            std::terminate();
        }
        started.acquire();
    }
};

Task<void> exerciseResults(BlockingPool& pool, WorkerHandle worker, bool& success) {
    auto value = co_await ruvia::runBlocking(pool, worker, [] { return 42; });
    if (!value.completed() || std::move(value).value() != 42) {
        co_return;
    }

    auto thrown = co_await ruvia::runBlocking(pool, worker, []() -> int {
        throw std::runtime_error("blocking work failed");
    });
    if (thrown.status() != BlockingStatus::kCompleted || !thrown.failed()) {
        co_return;
    }
    bool rethrown = false;
    try {
        static_cast<void>(std::move(thrown).value());
    } catch (const std::runtime_error&) {
        rethrown = true;
    }
    if (!rethrown) {
        co_return;
    }

    std::atomic_bool ran{false};
    auto empty = co_await ruvia::runBlocking(
        pool, worker, [flag = &ran] { flag->store(true); });
    if (!empty.completed() || !ran.load()) {
        co_return;
    }
    static_cast<void>(std::move(empty).value());

    // A moved-only result travels back by move, not by copy.
    auto owned = co_await ruvia::runBlocking(
        pool, worker, [] { return std::make_unique<int>(7); });
    if (!owned.completed()) {
        co_return;
    }
    const auto pointer = std::move(owned).value();
    success = pointer != nullptr && *pointer == 7;
}

// A wedged callable must not pin its caller forever: the wait has a deadline,
// even though the pool thread stays occupied until the callable returns.
Task<void> exerciseTimeout(
    BlockingPool& pool,
    WorkerHandle worker,
    ThreadGate& gate,
    bool& success) {
    auto timedOut = co_await ruvia::runBlocking(
        pool, worker, std::chrono::milliseconds(20), [&gate] {
            gate.release.acquire();
            return 1;
        });
    if (timedOut.status() != BlockingStatus::kTimedOut || timedOut.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(timedOut).value());
    } catch (const ruvia::BlockingOperationRejected& error) {
        rejected = error.status() == BlockingStatus::kTimedOut;
    }
    gate.release.release();

    // A deadline that is not reached behaves exactly like the untimed wait.
    auto inTime = co_await ruvia::runBlocking(
        pool, worker, std::chrono::seconds(30), [] { return 9; });
    success = rejected && inTime.completed() && std::move(inTime).value() == 9;
}

Task<void> countOnWorker(std::atomic_int& order, int& observed) {
    observed = order.fetch_add(1);
    co_return;
}

Task<void> blockThenCount(
    BlockingPool& pool,
    WorkerHandle worker,
    std::atomic_int& order,
    int& observed,
    bool& completed) {
    auto result = co_await ruvia::runBlocking(pool, worker, [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });
    completed = result.completed();
    observed = order.fetch_add(1);
}

// The point of the pool: while one handler waits on blocking work, the worker
// keeps running other coroutines instead of sitting inside the blocking call.
Task<void> exerciseWorkerStaysFree(
    BlockingPool& pool,
    WorkerHandle worker,
    bool& success) {
    std::atomic_int order{0};
    int blockingOrder = -1;
    int freeOrder = -1;
    bool blockingCompleted = false;

    ruvia::TaskScope scope(worker);
    scope.spawn(blockThenCount(pool, worker, order, blockingOrder, blockingCompleted));
    scope.spawn(countOnWorker(order, freeOrder));
    co_await scope.join();

    success = blockingCompleted && freeOrder == 0 && blockingOrder == 1;
}

// A pool that stops must not leave a suspended handler waiting forever: tasks
// discarded from the queue still answer their waiter.
Task<void> exerciseStoppedPool(
    BlockingPool& pool,
    WorkerHandle worker,
    ThreadGate& gate,
    bool& success) {
    std::thread stopper([&pool] {
        while (pool.stats().queued == 0) {
            std::this_thread::yield();
        }
        pool.stop();
    });

    auto result = co_await ruvia::runBlocking(pool, worker, [] { return 1; });
    stopper.join();
    gate.release.release();

    if (result.status() != BlockingStatus::kPoolStopped || result.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(result).value());
    } catch (const ruvia::BlockingOperationRejected& error) {
        rejected = error.status() == BlockingStatus::kPoolStopped;
    }
    // A stopped pool refuses new work rather than queueing it forever.
    auto refused = co_await ruvia::runBlocking(pool, worker, [] { return 2; });
    // A stopping pool is shutdown accounting, never the overload signal an
    // operator sizes the pool from.
    const auto stats = pool.stats();
    success = rejected && refused.status() == BlockingStatus::kPoolStopped &&
        stats.rejected == 0 && stats.discarded >= 2;
}

Task<void> exerciseQueueFull(
    BlockingPool& pool,
    WorkerHandle worker,
    ThreadGate& gate,
    bool& success) {
    // One thread is occupied and the single queue slot is taken, so this one
    // has nowhere to go.
    const auto queued = pool.submit([] {});
    auto result = co_await ruvia::runBlocking(pool, worker, [] { return 1; });
    gate.release.release();
    const auto stats = pool.stats();
    success = queued == BlockingSubmitStatus::kAccepted &&
        result.status() == BlockingStatus::kQueueFull &&
        stats.rejected == 1 && stats.discarded == 0;
}

Task<void> exerciseWorkerStopping(
    BlockingPool& pool,
    WorkerHandle worker,
    ThreadGate& gate,
    bool& success) {
    auto result = co_await ruvia::runBlocking(pool, worker, [&gate] {
        gate.release.acquire();
        return 5;
    });
    success = result.status() == BlockingStatus::kWorkerStopping &&
        !result.completed() && !result.failed();
}

// Offloading from a worker that has ALREADY stopped is the same shutdown
// outcome, not an exception the caller never asked for.
Task<void> exerciseStoppedWorker(
    BlockingPool& pool,
    WorkerHandle worker,
    std::atomic_bool& ran,
    bool& success) {
    auto result = co_await ruvia::runBlocking(
        pool, worker, [flag = &ran] { flag->store(true); return 1; });
    success = result.status() == BlockingStatus::kWorkerStopping;
}

}  // namespace

int main() {
    bool results = false;
    bool workerStaysFree = false;
    {
        BlockingPool pool(BlockingPoolOptions{.threadCount = 2});
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(exerciseResults(pool, worker, results)),
            asio::detached);
        ioContext.run();
        ioContext.restart();
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                exerciseWorkerStaysFree(pool, worker, workerStaysFree)),
            asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool timeout = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 2});
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                exerciseTimeout(pool, worker, gate, timeout)),
            asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool stoppedPool = false;
    {
        // The gate outlives the pool: the pool's destructor joins threads that
        // are still holding it.
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        gate.occupy(pool);
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                exerciseStoppedPool(pool, worker, gate, stoppedPool)),
            asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool queueFull = false;
    {
        ThreadGate gate;
        BlockingPool pool(
            BlockingPoolOptions{.threadCount = 1, .queueCapacity = 1});
        gate.occupy(pool);
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                exerciseQueueFull(pool, worker, gate, queueFull)),
            asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool workerStopping = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                exerciseWorkerStopping(pool, worker, gate, workerStopping)),
            asio::detached);
        asio::post(ioContext, [dispatcher] { dispatcher->close(); });
        ioContext.run();
        // The task is still holding a pool thread; release it only once nothing
        // is waiting for its result.
        gate.release.release();
        dispatcher->stopTimers();
    }

    bool stoppedWorker = false;
    std::atomic_bool stoppedWorkerTaskRan{false};
    {
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        asio::io_context ioContext;
        const auto dispatcher =
            std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        dispatcher->close();
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(exerciseStoppedWorker(
                pool, worker, stoppedWorkerTaskRan, stoppedWorker)),
            asio::detached);
        ioContext.run();
        dispatcher->stopTimers();
        // Nothing was submitted, so the pool never ran the callable.
        stoppedWorker = stoppedWorker && !stoppedWorkerTaskRan.load() &&
            pool.stats().completed == 0;
    }

    bool rejectsEmptyTask = false;
    {
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        try {
            static_cast<void>(pool.submit({}));
        } catch (const std::invalid_argument&) {
            rejectsEmptyTask = true;
        }
        pool.stop();
        pool.join();
        rejectsEmptyTask = rejectsEmptyTask &&
            pool.submit([] {}) == BlockingSubmitStatus::kStopped;
    }

    const bool allPassed = results && workerStaysFree && timeout &&
        stoppedPool && queueFull && workerStopping && stoppedWorker &&
        rejectsEmptyTask;
    return allPassed ? 0 : 1;
}
