#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>

#include "ruvia/core/PoolLeaseReleaseStatus.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"

namespace ruvia {

// Public, self-contained outcome contract for an asynchronous pool acquire.
class PoolWaiterResult final {
public:
    enum class Status { kAcquired,
        kTimedOut,
        kClosed,
        kCancelled };

    [[nodiscard]] constexpr Status status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool acquired() const noexcept {
        return status_ == Status::kAcquired;
    }
    [[nodiscard]] constexpr std::size_t index() const noexcept {
        return index_;
    }

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
        : status_(status),
          index_(index) {}
    Status status_;
    std::size_t index_;
};

// Single-worker owner of pool slot leases and their asynchronous wait queue.
// The worker handle, when supplied, is borrowed and must outlive this owner.
class PoolLeaseScheduler final {
public:
    explicit PoolLeaseScheduler(std::size_t poolSize,
        std::pmr::memory_resource* resource = nullptr);
    PoolLeaseScheduler(std::size_t poolSize, const WorkerHandle& worker,
        std::pmr::memory_resource* resource = nullptr);
    PoolLeaseScheduler(std::size_t, WorkerHandle&&, std::pmr::memory_resource* = nullptr) = delete;
    PoolLeaseScheduler(std::size_t, const WorkerHandle&&,
        std::pmr::memory_resource* = nullptr) = delete;

    PoolLeaseScheduler(const PoolLeaseScheduler&) = delete;
    PoolLeaseScheduler& operator=(const PoolLeaseScheduler&) = delete;
    PoolLeaseScheduler(PoolLeaseScheduler&&) = delete;
    PoolLeaseScheduler& operator=(PoolLeaseScheduler&&) = delete;
    ~PoolLeaseScheduler();

    [[nodiscard]] Task<PoolWaiterResult> acquire(
        std::optional<std::chrono::milliseconds> timeout);
    [[nodiscard]] Task<PoolWaiterResult> acquire(std::optional<std::chrono::milliseconds> timeout,
        StopToken stopToken, const WorkerHandle& worker);
    Task<PoolWaiterResult> acquire(
        std::optional<std::chrono::milliseconds>, StopToken, WorkerHandle&&) = delete;

    [[nodiscard]] PoolLeaseReleaseStatus release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia
