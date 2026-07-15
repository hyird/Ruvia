#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/PoolWaiterQueue.h"
#include "ruvia/core/memory/PmrResource.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

namespace ruvia::detail {

enum class PoolLeaseReleaseStatus : std::uint8_t {
    kReleased,
    kTransferredToWaiter,
    kInvalidSlot,
    kAlreadyReleased,
};

// Allocation-stable lease ownership for a single-worker connection pool.
// Concrete integrations map the typed acquire result to their own public error
// type and retain ownership of protocol connections; slot availability,
// timeout queueing, handoff, double-release rejection, and closure live here.
class PoolLeaseScheduler final {
public:
    PoolLeaseScheduler(
        std::size_t poolSize,
        std::pmr::memory_resource* resource = nullptr)
        : freeSlots_(pmrResourceOrDefault(resource)),
          busy_(pmrResourceOrDefault(resource)) {
        freeSlots_.reserve(poolSize);
        busy_.resize(poolSize, 0);
        for (std::size_t i = 0; i < poolSize; ++i) {
            freeSlots_.push_back(i);
        }
    }

    PoolLeaseScheduler(const PoolLeaseScheduler&) = delete;
    PoolLeaseScheduler& operator=(const PoolLeaseScheduler&) = delete;

    ~PoolLeaseScheduler() {
        if (reservedAcquires_ != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] Task<PoolWaiterResult> acquire(
        std::optional<std::chrono::milliseconds> timeout) {
        return acquireReserved(AcquireReservation(*this), timeout);
    }

private:
    class AcquireReservation final {
    public:
        explicit AcquireReservation(PoolLeaseScheduler& scheduler) noexcept
            : scheduler_(&scheduler) {
            ++scheduler_->reservedAcquires_;
        }
        ~AcquireReservation() {
            if (scheduler_ != nullptr) {
                --scheduler_->reservedAcquires_;
            }
        }

        AcquireReservation(const AcquireReservation&) = delete;
        AcquireReservation& operator=(const AcquireReservation&) = delete;
        AcquireReservation(AcquireReservation&& other) noexcept
            : scheduler_(std::exchange(other.scheduler_, nullptr)) {}
        AcquireReservation& operator=(AcquireReservation&&) = delete;

        [[nodiscard]] PoolLeaseScheduler& scheduler() const noexcept {
            return *scheduler_;
        }

    private:
        PoolLeaseScheduler* scheduler_;
    };

    [[nodiscard]] static Task<PoolWaiterResult> acquireReserved(
        AcquireReservation reservation,
        std::optional<std::chrono::milliseconds> timeout) {
        auto& scheduler = reservation.scheduler();
        if (scheduler.closing_) {
            co_return PoolWaiterResult::makeClosed();
        }
        if (!scheduler.freeSlots_.empty()) {
            const auto slot = scheduler.freeSlots_.back();
            scheduler.freeSlots_.pop_back();
            scheduler.busy_[slot] = 1;
            co_return PoolWaiterResult::makeAcquired(slot);
        }

        const auto deadline = timeout.has_value()
            ? std::chrono::steady_clock::now() + *timeout
            : std::chrono::steady_clock::time_point::max();
        PoolWaiter waiter(deadline);
        scheduler.waiters_.enqueue(waiter);
        struct WaiterGuard final {
            PoolWaiterQueue& queue;
            PoolWaiter& waiter;

            ~WaiterGuard() {
                queue.remove(waiter);
            }
        } guard{scheduler.waiters_, waiter};

        co_return co_await waiter;
    }

public:
    [[nodiscard]] PoolLeaseReleaseStatus release(std::size_t slot) noexcept {
        if (slot >= busy_.size()) {
            return PoolLeaseReleaseStatus::kInvalidSlot;
        }
        if (busy_[slot] == 0) {
            return PoolLeaseReleaseStatus::kAlreadyReleased;
        }
        if (!closing_ && waiters_.resumeNext(slot)) {
            return PoolLeaseReleaseStatus::kTransferredToWaiter;
        }
        busy_[slot] = 0;
        freeSlots_.push_back(slot);
        return PoolLeaseReleaseStatus::kReleased;
    }

    [[nodiscard]] bool close() noexcept {
        if (closing_) {
            return false;
        }
        closing_ = true;
        waiters_.closeAll();
        return true;
    }

    void scanDeadlines(
        std::chrono::steady_clock::time_point now) noexcept {
        waiters_.expireDeadlines(now);
    }

    [[nodiscard]] bool closing() const noexcept {
        return closing_;
    }

private:
    std::pmr::vector<std::size_t> freeSlots_;
    std::pmr::vector<std::uint8_t> busy_;
    PoolWaiterQueue waiters_;
    std::size_t reservedAcquires_{0};
    bool closing_{false};
};

}  // namespace ruvia::detail
