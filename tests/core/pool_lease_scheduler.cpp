#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/PoolLeaseScheduler.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"

namespace {

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{};
    std::size_t deallocations{};
    std::size_t liveBlocks{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* const block = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++allocations;
        ++liveBlocks;
        return block;
    }

    void do_deallocate(void* block, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        --liveBlocks;
        std::pmr::new_delete_resource()->deallocate(block, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

class AcquireProbeTask final {
public:
    struct promise_type final {
        [[nodiscard]] AcquireProbeTask get_return_object() noexcept {
            return AcquireProbeTask(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        [[nodiscard]] std::suspend_always final_suspend() const noexcept {
            return {};
        }

        void return_void() const noexcept {}

        [[noreturn]] void unhandled_exception() const noexcept {
            std::terminate();
        }
    };

    AcquireProbeTask(const AcquireProbeTask&) = delete;
    AcquireProbeTask& operator=(const AcquireProbeTask&) = delete;

    ~AcquireProbeTask() {
        handle_.destroy();
    }

    void start() const noexcept {
        handle_.resume();
    }

    [[nodiscard]] bool done() const noexcept {
        return handle_.done();
    }

private:
    explicit AcquireProbeTask(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    std::coroutine_handle<promise_type> handle_;
};

ruvia::Task<void> exerciseLeaseAndClose(
    ruvia::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    {
        auto discardedColdAcquire = scheduler.acquire(std::nullopt);
        static_cast<void>(discardedColdAcquire);
    }
    const auto first = co_await scheduler.acquire(std::nullopt);
    if (!first.acquired()) {
        co_return;
    }
    const auto index = first.index();
    if (scheduler.release(index) != ruvia::PoolLeaseReleaseStatus::kReleased ||
        scheduler.release(index) != ruvia::PoolLeaseReleaseStatus::kAlreadyReleased ||
        scheduler.release(index + 1) != ruvia::PoolLeaseReleaseStatus::kInvalidSlot) {
        co_return;
    }

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (!reacquired.acquired() || reacquired.index() != index) {
        co_return;
    }

    auto handoffStatus = ruvia::PoolLeaseReleaseStatus::kInvalidSlot;
    asio::post(ioContext,
        [&scheduler, &handoffStatus, index] { handoffStatus = scheduler.release(index); });
    const auto handedOff = co_await scheduler.acquire(std::nullopt);
    if (!handedOff.acquired() || handedOff.index() != index) {
        co_return;
    }

    asio::post(ioContext, [&scheduler] { (void)scheduler.close(); });
    const auto waitingAtClose = co_await scheduler.acquire(std::nullopt);
    if (handoffStatus != ruvia::PoolLeaseReleaseStatus::kTransferredToWaiter ||
        waitingAtClose.status() != ruvia::PoolWaiterResult::Status::kClosed ||
        !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    if (scheduler.release(index) != ruvia::PoolLeaseReleaseStatus::kReleased) {
        co_return;
    }
    const auto afterClose = co_await scheduler.acquire(std::nullopt);
    success = afterClose.status() == ruvia::PoolWaiterResult::Status::kClosed;
}

ruvia::Task<void> exerciseAcquireTimeout(
    ruvia::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    asio::post(ioContext,
        [&scheduler] { scheduler.scanDeadlines(std::chrono::steady_clock::time_point::max()); });
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = result.status() == ruvia::PoolWaiterResult::Status::kTimedOut;
}

ruvia::Task<void> exerciseWorkerBoundAcquireTimeout(
    ruvia::PoolLeaseScheduler& scheduler, bool& success) {
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = result.status() == ruvia::PoolWaiterResult::Status::kTimedOut;
}

ruvia::Task<void> exerciseSaturatedAcquireTimeout(
    ruvia::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    asio::post(ioContext, [&scheduler] {
        scheduler.scanDeadlines(std::chrono::steady_clock::now());
        (void)scheduler.close();
    });
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds::max());
    // A maximum positive timeout is effectively unbounded. Direct deadline
    // addition used to wrap it into the past, making the deadline scan win
    // with a false timeout instead of the subsequent close notification.
    success = result.status() == ruvia::PoolWaiterResult::Status::kClosed;
}

ruvia::Task<void> exerciseAcquireCancellation(ruvia::PoolLeaseScheduler& scheduler,
    asio::io_context& ioContext, const ruvia::WorkerHandle& worker, bool& success) {
    ruvia::StopSource source;
    asio::post(ioContext, [&scheduler, &source] {
        source.requestStop();
        (void)scheduler.close();
    });
    const auto result = co_await scheduler.acquire(std::nullopt, source.token());
    // Cancellation is committed before requestStop() returns. A same-stack
    // close must not replace it with kClosed while resumption is deferred.
    success = result.status() == ruvia::PoolWaiterResult::Status::kCancelled;
}

ruvia::Task<void> exercisePmrLifecycle(ruvia::PoolLeaseScheduler& scheduler,
    asio::io_context& ioContext, const ruvia::WorkerHandle& worker,
    CountingMemoryResource& resource, std::size_t baselineBlocks,
    ruvia::PoolWaiterResult& retainedResult, bool& success) {
    for (std::size_t i = 0; i < 32; ++i) {
        {
            auto discardedColdAcquire = scheduler.acquire(std::nullopt);
            static_cast<void>(discardedColdAcquire);
        }
        if (resource.liveBlocks != baselineBlocks) {
            co_return;
        }
    }

    const auto held = co_await scheduler.acquire(std::nullopt);
    if (!held.acquired()) {
        co_return;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        if (scheduler.release(held.index()) != ruvia::PoolLeaseReleaseStatus::kReleased) {
            co_return;
        }
        const auto acquired = co_await scheduler.acquire(std::nullopt);
        if (!acquired.acquired() || acquired.index() != held.index() ||
            resource.liveBlocks != baselineBlocks) {
            co_return;
        }
        retainedResult = acquired;
    }

    for (std::size_t i = 0; i < 32; ++i) {
        asio::post(ioContext, [&scheduler] {
            scheduler.scanDeadlines(std::chrono::steady_clock::time_point::max());
        });
        const auto timedOut = co_await scheduler.acquire(std::chrono::milliseconds(1));
        if (timedOut.status() != ruvia::PoolWaiterResult::Status::kTimedOut ||
            resource.liveBlocks != baselineBlocks) {
            co_return;
        }

        ruvia::StopSource source;
        asio::post(ioContext, [&source] { source.requestStop(); });
        const auto cancelled = co_await scheduler.acquire(std::nullopt, source.token());
        if (cancelled.status() != ruvia::PoolWaiterResult::Status::kCancelled ||
            resource.liveBlocks != baselineBlocks) {
            co_return;
        }
    }

    success = scheduler.release(held.index()) == ruvia::PoolLeaseReleaseStatus::kReleased &&
              resource.liveBlocks == baselineBlocks && retainedResult.acquired() &&
              retainedResult.index() == held.index();
}

AcquireProbeTask observeAcquireClosedAfterStaleCancellation(
    ruvia::PoolLeaseScheduler& scheduler, ruvia::StopToken stopToken,
    const ruvia::WorkerHandle& worker, bool& closed) {
    const auto result = co_await scheduler.acquire(std::nullopt, std::move(stopToken));
    closed = result.status() == ruvia::PoolWaiterResult::Status::kClosed;
}

bool exerciseCompletedAcquireIgnoresStalePostedCancellation(
    asio::io_context& ioContext, const ruvia::WorkerHandle& worker) {
    bool closed = false;
    {
        ruvia::PoolLeaseScheduler scheduler(0, worker);
        ruvia::StopSource source;
        auto probe =
            observeAcquireClosedAfterStaleCancellation(scheduler, source.token(), worker, closed);

        probe.start();
        if (probe.done()) {
            return false;
        }

        source.requestStop();
        if (probe.done()) {
            return false;
        }

        if (!scheduler.close() || !probe.done() || !closed) {
            return false;
        }
    }

    // The stop request above queued a worker cancellation before the acquire was
    // closed. It runs after the scheduler has been destroyed, so it must observe
    // the acquire's expired cancellation state instead of dereferencing the old
    // intrusive queue.
    ioContext.restart();
    ioContext.run();
    return true;
}

AcquireProbeTask observe_prepared_acquire(
    ruvia::Task<ruvia::PoolWaiterResult> task, ruvia::PoolWaiterResult::Status expected_status,
    bool& matched) {
    const auto result = co_await std::move(task);
    matched = result.status() == expected_status;
}

bool test_scheduler_owns_timeout_for_lazy_acquires() {
    bool success = true;
    for (const bool with_stop_token : {false, true}) {
        ruvia::PoolLeaseScheduler scheduler(0);
        std::optional<ruvia::Task<ruvia::PoolWaiterResult>> pending;
        {
            std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds(1);
            if (with_stop_token) {
                pending.emplace(scheduler.acquire(timeout, {}));
            } else {
                pending.emplace(scheduler.acquire(timeout));
            }
            timeout.reset();
        }
        bool timed_out = false;
        auto probe = observe_prepared_acquire(
            std::move(*pending), ruvia::PoolWaiterResult::Status::kTimedOut, timed_out);
        pending.reset();
        probe.start();
        success = success && !probe.done();
        scheduler.scanDeadlines(std::chrono::steady_clock::now() + std::chrono::seconds(1));
        success = success && probe.done() && timed_out;
        (void)scheduler.close();
    }
    return success;
}

bool test_scheduler_retains_worker_binding_for_lazy_acquires() {
    asio::io_context context;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(context, 1);
    CountingMemoryResource memory;
    bool success = true;
    {
        std::optional<ruvia::PoolLeaseScheduler> scheduler;
        std::optional<ruvia::Task<ruvia::PoolWaiterResult>> pending;
        ruvia::StopSource source;
        {
            auto temporary_worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
            scheduler.emplace(0, temporary_worker, &memory);
            pending.emplace(scheduler->acquire(std::nullopt, source.token()));
        }
        const auto baseline = memory.liveBlocks;
        bool cancelled = false;
        auto probe = observe_prepared_acquire(
            std::move(*pending), ruvia::PoolWaiterResult::Status::kCancelled, cancelled);
        pending.reset();
        probe.start();
        success = success && !probe.done();
        source.requestStop();
        dispatcher->runContext();
        success = success && probe.done() && cancelled && memory.liveBlocks == baseline;
        // The constructor also accepts a temporary handle directly; unstarted
        // operations release their reservation before the owning scheduler.
        {
            ruvia::PoolLeaseScheduler temporary_scheduler(
                1, ruvia::detail::WorkerHandleAccess::make(dispatcher), &memory);
            auto discarded = temporary_scheduler.acquire(std::nullopt, ruvia::StopSource{}.token());
        }
        success = success && memory.liveBlocks == baseline;
    }
    dispatcher->close();
    dispatcher->detachContext();
    return success && memory.liveBlocks == 0 && memory.allocations == memory.deallocations;
}

}  // namespace

int main() {
    asio::io_context ioContext;
    ruvia::PoolLeaseScheduler leaseScheduler(1);
    ruvia::PoolLeaseScheduler timeoutScheduler(0);
    ruvia::PoolLeaseScheduler saturatedTimeoutScheduler(0);
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 4);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::PoolLeaseScheduler workerTimeoutScheduler(0, worker);
    ruvia::PoolLeaseScheduler cancellationScheduler(0, worker);
    bool leaseSuccess = false;
    bool timeoutSuccess = false;
    bool workerTimeoutSuccess = false;
    bool saturatedTimeoutSuccess = false;
    bool cancellationSuccess = false;
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseLeaseAndClose(leaseScheduler, ioContext, leaseSuccess)),
        asio::detached);
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseAcquireTimeout(timeoutScheduler, ioContext, timeoutSuccess)),
        asio::detached);
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseWorkerBoundAcquireTimeout(workerTimeoutScheduler, workerTimeoutSuccess)),
        asio::detached);
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(exerciseSaturatedAcquireTimeout(
            saturatedTimeoutScheduler, ioContext, saturatedTimeoutSuccess)),
        asio::detached);
    asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(exerciseAcquireCancellation(
            cancellationScheduler, ioContext, worker, cancellationSuccess)),
        asio::detached);
    ioContext.run();
    const bool staleCancellationSuccess =
        exerciseCompletedAcquireIgnoresStalePostedCancellation(ioContext, worker);

    CountingMemoryResource resource;
    bool pmrLifecycleSuccess = false;
    ruvia::PoolWaiterResult retainedResult = ruvia::PoolWaiterResult::makeClosed();
    {
        ruvia::PoolLeaseScheduler scheduler(1, worker, &resource);
        const auto baselineBlocks = resource.liveBlocks;
        ioContext.restart();
        asio::co_spawn(ioContext,
            ruvia::detail::taskAsAwaitable(exercisePmrLifecycle(scheduler, ioContext, worker,
                resource, baselineBlocks, retainedResult, pmrLifecycleSuccess)),
            asio::detached);
        ioContext.run();
        pmrLifecycleSuccess = pmrLifecycleSuccess && resource.liveBlocks == baselineBlocks;
    }
    pmrLifecycleSuccess = pmrLifecycleSuccess && resource.liveBlocks == 0 &&
                          resource.allocations == resource.deallocations;

    dispatcher->close();
    return leaseSuccess && timeoutSuccess && workerTimeoutSuccess && saturatedTimeoutSuccess &&
                   cancellationSuccess && staleCancellationSuccess && pmrLifecycleSuccess &&
                   test_scheduler_owns_timeout_for_lazy_acquires() &&
                   test_scheduler_retains_worker_binding_for_lazy_acquires()
               ? 0
               : 1;
}
