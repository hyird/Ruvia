#pragma once

#include <cstddef>
#include <variant>

#include "ruvia/core/PoolLeaseReleaseStatus.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"

namespace ruvia {

// Public, self-contained outcome contract for an asynchronous pool acquire.
class PoolWaiterResult final {
public:
    enum class Status { kAcquired, kTimedOut, kClosed, kCancelled };

    [[nodiscard]] constexpr Status status() const noexcept { return status_; }
    [[nodiscard]] constexpr bool acquired() const noexcept { return status_ == Status::kAcquired; }
    [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }

    [[nodiscard]] static constexpr PoolWaiterResult makeAcquired(std::size_t index) noexcept {
        return PoolWaiterResult(Status::kAcquired, index);
    }
    [[nodiscard]] static constexpr PoolWaiterResult makeTimedOut() noexcept {
        return PoolWaiterResult(Status::kTimedOut, 0);
    }
    [[nodiscard]] static constexpr PoolWaiterResult makeClosed() noexcept {
        return PoolWaiterResult(Status::kClosed, 0);
    }
    [[nodiscard]] static constexpr PoolWaiterResult makeCancelled() noexcept {
        return PoolWaiterResult(Status::kCancelled, 0);
    }

private:
    constexpr PoolWaiterResult(Status status, std::size_t index) noexcept
        : status_(status), index_(index) {}
    Status status_;
    std::size_t index_;
};

class PoolLeaseScheduler final : public detail::PoolLeaseScheduler {
public:
    using detail::PoolLeaseScheduler::PoolLeaseScheduler;

    [[nodiscard]] Task<PoolWaiterResult> acquire(
        std::optional<std::chrono::milliseconds> timeout) {
        return convert(detail::PoolLeaseScheduler::acquire(timeout));
    }
    [[nodiscard]] Task<PoolWaiterResult> acquire(std::optional<std::chrono::milliseconds> timeout,
        StopToken stopToken, const WorkerHandle& worker) {
        return convert(detail::PoolLeaseScheduler::acquire(timeout, stopToken, worker));
    }
    Task<PoolWaiterResult> acquire(
        std::optional<std::chrono::milliseconds>, StopToken, WorkerHandle&&) = delete;

private:
    static Task<PoolWaiterResult> convert(Task<detail::PoolWaiterResult> task) {
        const auto& result = co_await std::move(task);
        if (const auto* acquired = result.acquired()) co_return PoolWaiterResult::makeAcquired(acquired->index());
        if (result.timedOut()) co_return PoolWaiterResult::makeTimedOut();
        if (result.closed()) co_return PoolWaiterResult::makeClosed();
        co_return PoolWaiterResult::makeCancelled();
    }
};
}  // namespace ruvia
