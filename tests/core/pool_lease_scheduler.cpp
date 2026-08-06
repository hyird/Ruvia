#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/pool/PoolLeaseScheduler.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <memory>
#include <optional>

namespace {

ruvia::Task<void> exerciseLeaseAndClose(ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
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
    if (scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kReleased || scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kAlreadyReleased || scheduler.release(index + 1) != ruvia::detail::PoolLeaseReleaseStatus::kInvalidSlot) {
        co_return;
    }

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (reacquired.acquired() == nullptr || reacquired.acquired()->index() != index) {
        co_return;
    }

    auto handoffStatus = ruvia::detail::PoolLeaseReleaseStatus::kInvalidSlot;
    asio::post(ioContext, [&scheduler, &handoffStatus, index] { handoffStatus = scheduler.release(index); });
    const auto handedOff = co_await scheduler.acquire(std::nullopt);
    if (handedOff.acquired() == nullptr || handedOff.acquired()->index() != index) {
        co_return;
    }

    asio::post(ioContext, [&scheduler] { (void)scheduler.close(); });
    const auto waitingAtClose = co_await scheduler.acquire(std::nullopt);
    if (handoffStatus != ruvia::detail::PoolLeaseReleaseStatus::kTransferredToWaiter || waitingAtClose.closed() == nullptr || !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    if (scheduler.release(index) != ruvia::detail::PoolLeaseReleaseStatus::kReleased) {
        co_return;
    }
    const auto afterClose = co_await scheduler.acquire(std::nullopt);
    success = afterClose.closed() != nullptr;
}

ruvia::Task<void> exerciseAcquireTimeout(ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
    asio::post(ioContext, [&scheduler] { scheduler.scanDeadlines(std::chrono::steady_clock::time_point::max()); });
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = result.timedOut() != nullptr;
}

ruvia::Task<void> exerciseSaturatedAcquireTimeout(ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, bool& success) {
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

ruvia::Task<void> exerciseAcquireCancellation(ruvia::detail::PoolLeaseScheduler& scheduler, asio::io_context& ioContext, const ruvia::WorkerHandle& worker, bool& success) {
    ruvia::detail::StopSource source;
    asio::post(ioContext, [&source] { source.requestStop(); });
    const auto result = co_await scheduler.acquire(std::nullopt, source.token(), worker);
    success = result.cancelled() != nullptr;
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
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseLeaseAndClose(leaseScheduler, ioContext, leaseSuccess)), asio::detached);
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseAcquireTimeout(timeoutScheduler, ioContext, timeoutSuccess)), asio::detached);
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseSaturatedAcquireTimeout(saturatedTimeoutScheduler, ioContext, saturatedTimeoutSuccess)), asio::detached);
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseAcquireCancellation(cancellationScheduler, ioContext, worker, cancellationSuccess)), asio::detached);
    ioContext.run();
    dispatcher->close();
    return leaseSuccess && timeoutSuccess && saturatedTimeoutSuccess && cancellationSuccess ? 0 : 1;
}
