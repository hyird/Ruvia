#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/pool/PoolLeaseScheduler.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>

namespace {

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
    ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    {
        auto discardedColdAcquire = scheduler.acquire(std::nullopt);
        static_cast<void>(discardedColdAcquire);
    }
    const auto first = co_await scheduler.acquire(std::nullopt);
    const auto* firstLease = first.acquired();
    if (firstLease == nullptr) {
        co_return;
    }
    const auto index = firstLease->index();
    if (scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kReleased ||
        scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kAlreadyReleased ||
        scheduler.release(index + 1) != ruvia::detail::PoolLeaseReleaseStatus::kInvalidSlot) {
        co_return;
    }

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (reacquired.acquired() == nullptr || reacquired.acquired()->index() != index) {
        co_return;
    }

    auto handoffStatus = ruvia::detail::PoolLeaseReleaseStatus::kInvalidSlot;
    asio::post(ioContext,
        [&scheduler, &handoffStatus, index] { handoffStatus = scheduler.release(index); });
    const auto handedOff = co_await scheduler.acquire(std::nullopt);
    if (handedOff.acquired() == nullptr || handedOff.acquired()->index() != index) {
        co_return;
    }

    asio::post(ioContext, [&scheduler] { (void)scheduler.close(); });
    const auto waitingAtClose = co_await scheduler.acquire(std::nullopt);
    if (handoffStatus != ruvia::detail::PoolLeaseReleaseStatus::kTransferredToWaiter ||
        waitingAtClose.closed() == nullptr || !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    if (scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kReleased) {
        co_return;
    }
    const auto afterClose = co_await scheduler.acquire(std::nullopt);
    success = afterClose.closed() != nullptr;
}

ruvia::Task<void> exerciseAcquireTimeout(
    ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    asio::post(ioContext,
        [&scheduler] { scheduler.scanDeadlines(std::chrono::steady_clock::time_point::max()); });
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = result.timedOut() != nullptr;
}

ruvia::Task<void> exerciseSaturatedAcquireTimeout(
    ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    asio::post(ioContext, [&scheduler] {
        scheduler.scanDeadlines(std::chrono::steady_clock::now());
        (void)scheduler.close();
    });
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds::max());
    // A maximum positive timeout is effectively unbounded. Direct deadline
    // addition used to wrap it into the past, making the deadline scan win
    // with a false timeout instead of the subsequent close notification.
    success = result.closed() != nullptr;
}

ruvia::Task<void> exerciseAcquireCancellation(ruvia::detail::PoolLeaseScheduler& scheduler,
    asio::io_context& ioContext, const ruvia::WorkerHandle& worker, bool& success) {
    ruvia::StopSource source;
    asio::post(ioContext, [&scheduler, &source] {
        source.requestStop();
        (void)scheduler.close();
    });
    const auto result = co_await scheduler.acquire(std::nullopt, source.token(), worker);
    // Cancellation is committed before requestStop() returns. A same-stack
    // close must not replace it with kClosed while resumption is deferred.
    success = result.cancelled() != nullptr;
}

AcquireProbeTask observeAcquireClosedAfterStaleCancellation(
    ruvia::detail::PoolLeaseScheduler& scheduler, ruvia::StopToken stopToken,
    const ruvia::WorkerHandle& worker, bool& closed) {
    const auto result = co_await scheduler.acquire(std::nullopt, std::move(stopToken), worker);
    closed = result.closed() != nullptr;
}

bool exerciseCompletedAcquireIgnoresStalePostedCancellation(
    asio::io_context& ioContext, const ruvia::WorkerHandle& worker) {
    bool closed = false;
    {
        ruvia::detail::PoolLeaseScheduler scheduler(0);
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

}  // namespace

int main() {
    asio::io_context ioContext;
    ruvia::detail::PoolLeaseScheduler leaseScheduler(1);
    ruvia::detail::PoolLeaseScheduler timeoutScheduler(0);
    ruvia::detail::PoolLeaseScheduler saturatedTimeoutScheduler(0);
    ruvia::detail::PoolLeaseScheduler cancellationScheduler(0);
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 4);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool leaseSuccess = false;
    bool timeoutSuccess = false;
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
    dispatcher->close();
    return leaseSuccess && timeoutSuccess && saturatedTimeoutSuccess && cancellationSuccess &&
                   staleCancellationSuccess
               ? 0
               : 1;
}
