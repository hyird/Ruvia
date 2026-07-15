#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/PoolLeaseScheduler.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <optional>

namespace {

ruvia::Task<void> exerciseLeaseAndClose(
    ruvia::detail::PoolLeaseScheduler& scheduler,
    asio::io_context& ioContext,
    bool& success) {
    const auto first = co_await scheduler.acquire(std::nullopt);
    const auto* firstLease = first.acquired();
    if (firstLease == nullptr) {
        co_return;
    }
    const auto index = firstLease->index();
    scheduler.release(index);
    scheduler.release(index);  // duplicate release must not duplicate the slot

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (reacquired.acquired() == nullptr ||
        reacquired.acquired()->index() != index) {
        co_return;
    }

    asio::post(ioContext, [&scheduler] {
        (void)scheduler.close();
    });
    const auto waitingAtClose = co_await scheduler.acquire(std::nullopt);
    if (waitingAtClose.closed() == nullptr || !scheduler.closing() ||
        scheduler.close()) {
        co_return;
    }
    scheduler.release(index);
    const auto afterClose = co_await scheduler.acquire(std::nullopt);
    success = afterClose.closed() != nullptr;
}

ruvia::Task<void> exerciseAcquireTimeout(
    ruvia::detail::PoolLeaseScheduler& scheduler,
    asio::io_context& ioContext,
    bool& success) {
    asio::post(ioContext, [&scheduler] {
        scheduler.scanDeadlines(
            std::chrono::steady_clock::time_point::max());
    });
    const auto result = co_await scheduler.acquire(
        std::chrono::milliseconds(1));
    success = result.timedOut() != nullptr;
}

}  // namespace

int main() {
    asio::io_context ioContext;
    ruvia::detail::PoolLeaseScheduler leaseScheduler(1);
    ruvia::detail::PoolLeaseScheduler timeoutScheduler(0);
    bool leaseSuccess = false;
    bool timeoutSuccess = false;
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseLeaseAndClose(
                leaseScheduler, ioContext, leaseSuccess)),
        asio::detached);
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            exerciseAcquireTimeout(
                timeoutScheduler, ioContext, timeoutSuccess)),
        asio::detached);
    ioContext.run();
    return leaseSuccess && timeoutSuccess ? 0 : 1;
}
