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
#include <future>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

template <typename T>
concept HasRvalueBlockingError = requires(T&& result) { std::move(result).error(); };

static_assert(!std::is_constructible_v<ruvia::BlockingResult<int>, ruvia::BlockingStatus>);
static_assert(!std::is_constructible_v<ruvia::BlockingOperationRejected, ruvia::BlockingStatus>);
static_assert(!HasRvalueBlockingError<ruvia::BlockingResult<int>>);

namespace {

using ruvia::BlockingPool;
using ruvia::BlockingPoolOptions;

[[nodiscard]] bool blockingPoolDefaultsAreBoundedByCpuPolicy() {
    BlockingPool pool;
    const bool valid = pool.threadCount() >= 2 && pool.threadCount() <= 8 && pool.queueCapacity() == pool.threadCount() * 64;
    pool.stop();
    pool.join();
    return valid;
}
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

class ThrowOnSecondMove final {
public:
    ThrowOnSecondMove() = default;
    ThrowOnSecondMove(const ThrowOnSecondMove&) = delete;
    ThrowOnSecondMove& operator=(const ThrowOnSecondMove&) = delete;
    ThrowOnSecondMove(ThrowOnSecondMove&&) {
        if (moves.fetch_add(1, std::memory_order_relaxed) + 1 == 2) {
            throw std::runtime_error("second move failed");
        }
    }
    ThrowOnSecondMove& operator=(ThrowOnSecondMove&&) = delete;

    static inline std::atomic_int moves{0};
};

Task<void> exerciseResults(BlockingPool& pool, WorkerHandle worker, bool& success) {
    auto value = co_await ruvia::tryRunBlocking(pool, worker, [] { return 42; });
    if (!value.completed() || std::move(value).value() != 42) {
        co_return;
    }

    auto thrown = co_await ruvia::tryRunBlocking(pool, worker, []() -> int { throw std::runtime_error("blocking work failed"); });
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
    auto empty = co_await ruvia::tryRunBlocking(pool, worker, [flag = &ran] { flag->store(true); });
    if (!empty.completed() || !ran.load()) {
        co_return;
    }
    static_cast<void>(std::move(empty).value());

    // A moved-only result travels back by move, not by copy.
    auto owned = co_await ruvia::tryRunBlocking(pool, worker, [] { return std::make_unique<int>(7); });
    if (!owned.completed()) {
        co_return;
    }
    const auto pointer = std::move(owned).value();
    if (pointer == nullptr || *pointer != 7) {
        co_return;
    }

    const auto direct = co_await ruvia::runBlocking(pool, worker, [] { return 11; });
    bool directRethrew = false;
    try {
        static_cast<void>(co_await ruvia::runBlocking(pool, worker, []() -> int { throw std::runtime_error("direct blocking failure"); }));
    } catch (const std::runtime_error& error) {
        directRethrew = std::string_view(error.what()) == "direct blocking failure";
    }
    success = direct == 11 && directRethrew;
}

Task<void> exerciseThrowingMoveResult(BlockingPool& pool, WorkerHandle worker, bool& success) {
    ThrowOnSecondMove::moves.store(0, std::memory_order_relaxed);
    auto result = co_await ruvia::tryRunBlocking(pool, worker, std::chrono::seconds(1), [] { return ThrowOnSecondMove{}; });
    if (result.status() != BlockingStatus::kCompleted || !result.failed() || result.error() == nullptr) {
        co_return;
    }
    try {
        static_cast<void>(std::move(result).value());
    } catch (const std::runtime_error& error) {
        success = std::string_view(error.what()) == "second move failed";
    }
}

Task<void> exerciseCancellation(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, ruvia::StopSource& source, std::atomic_bool& callableFinished, bool& success) {
    auto result = co_await ruvia::tryRunBlocking(pool, worker, std::chrono::seconds(30), source.token(), [&] {
        gate.started.release();
        gate.release.acquire();
        callableFinished.store(true, std::memory_order_release);
        return 12;
    });
    if (result.status() != BlockingStatus::kCancelled || result.completed() || result.failed()) {
        gate.release.release();
        co_return;
    }
    try {
        static_cast<void>(std::move(result).value());
    } catch (const ruvia::BlockingOperationRejected& error) {
        success = error.status() == BlockingStatus::kCancelled;
    }
    gate.release.release();
}

// A wedged callable must not pin its caller forever: the wait has a deadline,
// even though the pool thread stays occupied until the callable returns.
Task<void> exerciseTimeout(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, bool& success) {
    auto timedOut = co_await ruvia::tryRunBlocking(pool, worker, std::chrono::milliseconds(20), [&gate] {
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
    auto inTime = co_await ruvia::tryRunBlocking(pool, worker, std::chrono::seconds(30), [] { return 9; });
    success = rejected && inTime.completed() && std::move(inTime).value() == 9;
}

Task<void> exerciseSaturatingTimeout(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, bool& success) {
    // Keep the pool occupied so a wrapped timeout cannot hide behind a task
    // that happens to finish before the receiver arms its deadline.
    std::thread releaser([&pool, &gate] {
        while (pool.stats().queued == 0) {
            std::this_thread::yield();
        }
        gate.release.release();
    });

    auto result = co_await ruvia::tryRunBlocking(pool, worker, std::chrono::hours::max(), [] { return 17; });
    releaser.join();
    success = result.completed() && std::move(result).value() == 17;
}

Task<void> countOnWorker(std::atomic_int& order, int& observed) {
    observed = order.fetch_add(1);
    co_return;
}

Task<void> blockThenCount(BlockingPool& pool, WorkerHandle worker, std::atomic_int& order, int& observed, bool& completed) {
    auto result = co_await ruvia::tryRunBlocking(pool, worker, [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    completed = result.completed();
    observed = order.fetch_add(1);
}

// The point of the pool: while one handler waits on blocking work, the worker
// keeps running other coroutines instead of sitting inside the blocking call.
Task<void> exerciseWorkerStaysFree(BlockingPool& pool, WorkerHandle worker, bool& success) {
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
Task<void> exerciseStoppedPool(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, bool& success) {
    std::thread stopper([&pool] {
        while (pool.stats().queued == 0) {
            std::this_thread::yield();
        }
        pool.stop();
    });

    auto result = co_await ruvia::tryRunBlocking(pool, worker, [] { return 1; });
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
    auto refused = co_await ruvia::tryRunBlocking(pool, worker, [] { return 2; });
    // A stopping pool is shutdown accounting, never the overload signal an
    // operator sizes the pool from.
    const auto stats = pool.stats();
    success = rejected && refused.status() == BlockingStatus::kPoolStopped && stats.rejected == 0 && stats.discarded >= 2;
}

Task<void> exerciseQueueFull(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, bool& success) {
    // One thread is occupied and the single queue slot is taken, so this one
    // has nowhere to go.
    const auto queued = pool.submit([] {});
    auto result = co_await ruvia::tryRunBlocking(pool, worker, [] { return 1; });
    gate.release.release();
    const auto stats = pool.stats();
    success = queued == BlockingSubmitStatus::kAccepted && result.status() == BlockingStatus::kQueueFull && stats.rejected == 1 && stats.discarded == 0;
}

Task<void> exerciseWorkerStopping(BlockingPool& pool, WorkerHandle worker, ThreadGate& gate, bool& success) {
    auto result = co_await ruvia::tryRunBlocking(pool, worker, [&gate] {
        gate.release.acquire();
        return 5;
    });
    success = result.status() == BlockingStatus::kWorkerStopping && !result.completed() && !result.failed();
}

// Offloading from a worker that has ALREADY stopped is the same shutdown
// outcome, not an exception the caller never asked for.
Task<void> exerciseStoppedWorker(BlockingPool& pool, WorkerHandle worker, std::atomic_bool& ran, bool& success) {
    auto result = co_await ruvia::tryRunBlocking(pool, worker, [flag = &ran] {
        flag->store(true);
        return 1;
    });
    success = result.status() == BlockingStatus::kWorkerStopping;
}

bool testDestructionDoesNotJoinRunningCallable() {
    auto startedPromise = std::make_shared<std::promise<void>>();
    auto started = startedPromise->get_future();
    auto finishedPromise = std::make_shared<std::promise<void>>();
    auto finished = finishedPromise->get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::promise<void> destroyedPromise;
    auto destroyed = destroyedPromise.get_future();

    auto pool = std::make_unique<BlockingPool>(BlockingPoolOptions{.threadCount = 1});
    if (pool->submit([started = std::move(startedPromise), finished = std::move(finishedPromise), release] {
            started->set_value();
            release.wait();
            finished->set_value();
        }) != BlockingSubmitStatus::kAccepted) {
        return false;
    }
    started.wait();

    std::thread destroyer([pool = std::move(pool), &destroyedPromise]() mutable {
        pool.reset();
        destroyedPromise.set_value();
    });
    const bool returnedBeforeCallable = destroyed.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;

    releasePromise.set_value();
    destroyer.join();
    const bool callableFinished = finished.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    return returnedBeforeCallable && callableFinished;
}

bool testJoinStopsAndWaitsForRunningCallable() {
    auto startedPromise = std::make_shared<std::promise<void>>();
    auto started = startedPromise->get_future();
    std::promise<void> releasePromise;
    auto release = releasePromise.get_future().share();
    std::promise<void> joinedPromise;
    auto joined = joinedPromise.get_future();

    BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
    if (pool.submit([started = std::move(startedPromise), release] {
            started->set_value();
            release.wait();
        }) != BlockingSubmitStatus::kAccepted) {
        return false;
    }
    started.wait();

    std::thread joiner([&pool, &joinedPromise] {
        pool.join();
        joinedPromise.set_value();
    });
    const bool joinWaited = joined.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;

    releasePromise.set_value();
    joiner.join();
    return joinWaited && joined.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

bool testJoinRejectsPoolThreadBeforeStopping() {
    BlockingPool pool(BlockingPoolOptions{.threadCount = 1, .queueCapacity = 1});
    std::promise<bool> completed;
    auto result = completed.get_future();
    if (pool.submit([&] {
            bool rejected = false;
            try {
                pool.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) == "cannot join a blocking pool from one of its threads";
            }
            const auto stillAccepting = pool.submit([] {}) == BlockingSubmitStatus::kAccepted;
            completed.set_value(rejected && stillAccepting);
        }) != BlockingSubmitStatus::kAccepted) {
        return false;
    }
    const bool rejected = result.get();
    pool.join();
    return rejected;
}

}  // namespace

int main() {
    bool results = false;
    bool workerStaysFree = false;
    bool throwingMoveResult = false;
    {
        BlockingPool pool(BlockingPoolOptions{.threadCount = 2});
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseResults(pool, worker, results)), asio::detached);
        ioContext.run();
        ioContext.restart();
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseWorkerStaysFree(pool, worker, workerStaysFree)), asio::detached);
        ioContext.run();
        ioContext.restart();
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseThrowingMoveResult(pool, worker, throwingMoveResult)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool cancelled = false;
    std::atomic_bool cancelledCallableFinished{false};
    {
        ThreadGate gate;
        ruvia::StopSource source;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        std::thread canceller([&] {
            gate.started.acquire();
            source.requestStop();
        });
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseCancellation(pool, worker, gate, source, cancelledCallableFinished, cancelled)), asio::detached);
        ioContext.run();
        canceller.join();
        pool.join();
        dispatcher->close();
        dispatcher->stopTimers();
    }
    cancelled = cancelled && cancelledCallableFinished.load(std::memory_order_acquire);

    bool timeout = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 2});
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseTimeout(pool, worker, gate, timeout)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool saturatingTimeout = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        gate.occupy(pool);
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseSaturatingTimeout(pool, worker, gate, saturatingTimeout)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool stoppedPool = false;
    {
        // The gate outlives the pool: the pool's destructor detaches the thread
        // while the callable is still holding it.
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        gate.occupy(pool);
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseStoppedPool(pool, worker, gate, stoppedPool)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool queueFull = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1, .queueCapacity = 1});
        gate.occupy(pool);
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseQueueFull(pool, worker, gate, queueFull)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool workerStopping = false;
    {
        ThreadGate gate;
        BlockingPool pool(BlockingPoolOptions{.threadCount = 1});
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseWorkerStopping(pool, worker, gate, workerStopping)), asio::detached);
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
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        dispatcher->close();
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseStoppedWorker(pool, worker, stoppedWorkerTaskRan, stoppedWorker)), asio::detached);
        ioContext.run();
        dispatcher->stopTimers();
        // Nothing was submitted, so the pool never ran the callable.
        stoppedWorker = stoppedWorker && !stoppedWorkerTaskRan.load() && pool.stats().completed == 0;
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
        rejectsEmptyTask = rejectsEmptyTask && pool.submit([] {}) == BlockingSubmitStatus::kPoolStopped;
    }

    const bool defaultSizing = blockingPoolDefaultsAreBoundedByCpuPolicy();
    const bool destructionDoesNotJoin = testDestructionDoesNotJoinRunningCallable();
    const bool joinStopsAndWaits = testJoinStopsAndWaitsForRunningCallable();
    const bool joinRejectsPoolThread = testJoinRejectsPoolThreadBeforeStopping();
    const bool allPassed = defaultSizing && results && workerStaysFree && throwingMoveResult && cancelled && timeout && saturatingTimeout && stoppedPool && queueFull && workerStopping && stoppedWorker && rejectsEmptyTask && destructionDoesNotJoin && joinStopsAndWaits && joinRejectsPoolThread;
    return allPassed ? 0 : 1;
}
