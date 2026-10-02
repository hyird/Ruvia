#include "ruvia/core/PoolLeaseScheduler.h"

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/detail/pool/PoolWaiterQueue.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia {

class PoolLeaseScheduler::Impl final {
public:
    Impl(std::size_t poolSize, const WorkerHandle* worker, std::pmr::memory_resource* resource)
        : worker_(worker != nullptr ? *worker : WorkerHandle{}),
          freeSlots_(detail::pmrResourceOrDefault(resource)),
          busy_(detail::pmrResourceOrDefault(resource)),
          waiterState_(std::allocate_shared<WaiterState>(
              std::pmr::polymorphic_allocator<WaiterState>(detail::processResource()))) {
        freeSlots_.reserve(poolSize);
        busy_.resize(poolSize, 0);
        for (std::size_t i = 0; i < poolSize; ++i) {
            freeSlots_.push_back(i);
        }
    }

    ~Impl() {
        if (reservedAcquires_ != 0) {
            std::terminate();
        }
    }

    struct WaiterState final : std::enable_shared_from_this<WaiterState> {
        detail::PoolWaiterQueue queue;

        void requestCancellation(const WorkerHandle& worker, std::uint64_t waiterId) {
            if (worker.isCurrent()) {
                std::coroutine_handle<> continuation;
                if (!queue.commitCancellation(waiterId, continuation) || !continuation) {
                    return;
                }
                detail::WorkerHandleAccess::deferOrTerminate(
                    worker, [continuation] { continuation.resume(); });
                return;
            }
            auto retained = shared_from_this();
            detail::WorkerHandleAccess::deferOrTerminate(
                worker, [retained = std::move(retained), waiterId] {
                    (void)retained->queue.cancel(waiterId);
                });
        }
    };

    class AcquireReservation final {
    public:
        explicit AcquireReservation(Impl& owner) noexcept
            : owner_(&owner) {
            ++owner_->reservedAcquires_;
        }
        ~AcquireReservation() {
            if (owner_ != nullptr) {
                --owner_->reservedAcquires_;
            }
        }
        AcquireReservation(const AcquireReservation&) = delete;
        AcquireReservation& operator=(const AcquireReservation&) = delete;
        AcquireReservation(AcquireReservation&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)) {}
        AcquireReservation& operator=(AcquireReservation&&) = delete;

        [[nodiscard]] Impl& owner() const noexcept {
            return *owner_;
        }

    private:
        Impl* owner_;
    };

    [[nodiscard]] static Task<PoolWaiterResult> acquireReserved(AcquireReservation reservation,
        std::optional<std::chrono::milliseconds> timeout, StopToken stopToken,
        const WorkerHandle* worker) {
        auto& owner = reservation.owner();
        if (stopToken.stoppable() && (worker == nullptr || !worker->valid())) {
            throw std::logic_error("cancellable pool acquire requires a valid worker");
        }
        if (owner.closing_) {
            co_return PoolWaiterResult::makeClosed();
        }
        if (stopToken.stopRequested()) {
            co_return PoolWaiterResult::makeCancelled();
        }
        if (!owner.freeSlots_.empty()) {
            const auto slot = owner.freeSlots_.back();
            owner.freeSlots_.pop_back();
            owner.busy_[slot] = 1;
            co_return PoolWaiterResult::makeAcquired(slot);
        }

        const auto deadline = timeout.has_value() ? detail::workerTimerDeadlineAfter(*timeout)
                                                  : std::chrono::steady_clock::time_point::max();
        auto waiterId = ++owner.nextWaiterId_;
        if (waiterId == 0) {
            waiterId = ++owner.nextWaiterId_;
        }
        detail::PoolWaiter waiter(deadline, waiterId);
        auto* const waiterState = owner.waiterState_.get();
        waiterState->queue.enqueue(waiter);
        struct WaiterGuard final {
            detail::PoolWaiterQueue& queue;
            detail::PoolWaiter& waiter;
            ~WaiterGuard() {
                queue.remove(waiter);
            }
        } guard{waiterState->queue, waiter};

        detail::WorkerTimerRegistration deadlineTimer;
        if (timeout.has_value() && worker != nullptr && worker->valid()) {
            detail::WorkerHandleAccess::scheduleTimer(*worker, deadlineTimer, deadline,
                [waiterState, waiterId](detail::WorkerTimerOutcome outcome) noexcept {
                    if (outcome == detail::WorkerTimerOutcome::kExpired) {
                        (void)waiterState->queue.expire(waiterId);
                    }
                });
        }

        auto stopRegistration = stopToken.registerCallback([worker, waiterState, waiterId] {
            waiterState->requestCancellation(*worker, waiterId);
        });
        if (stopToken.stoppable() && stopToken.stopRequested()) {
            (void)waiterState->queue.cancel(waiterId);
        }

        const auto& result = co_await waiter;
        deadlineTimer.cancel();
        stopRegistration.reset();
        if (result.acquired() != nullptr) {
            co_return PoolWaiterResult::makeAcquired(result.acquired()->index());
        }
        if (result.timedOut() != nullptr) {
            co_return PoolWaiterResult::makeTimedOut();
        }
        if (result.closed() != nullptr) {
            co_return PoolWaiterResult::makeClosed();
        }
        co_return PoolWaiterResult::makeCancelled();
    }

    [[nodiscard]] PoolLeaseReleaseStatus release(std::size_t slot) noexcept {
        if (slot >= busy_.size()) {
            return PoolLeaseReleaseStatus::kInvalidSlot;
        }
        if (busy_[slot] == 0) {
            return PoolLeaseReleaseStatus::kAlreadyReleased;
        }
        if (!closing_ && waiterState_->queue.resumeNext(slot)) {
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
        waiterState_->queue.closeAll();
        return true;
    }

    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
        waiterState_->queue.expireDeadlines(now);
    }

    WorkerHandle worker_;
    std::pmr::vector<std::size_t> freeSlots_;
    std::pmr::vector<std::uint8_t> busy_;
    std::shared_ptr<WaiterState> waiterState_;
    std::size_t reservedAcquires_{0};
    std::uint64_t nextWaiterId_{0};
    bool closing_{false};
};

PoolLeaseScheduler::PoolLeaseScheduler(
    std::size_t poolSize, std::pmr::memory_resource* resource)
    : impl_(std::make_unique<Impl>(poolSize, nullptr, resource)) {}

PoolLeaseScheduler::PoolLeaseScheduler(std::size_t poolSize, const WorkerHandle& worker,
    std::pmr::memory_resource* resource) {
    if (!worker.valid()) {
        throw std::invalid_argument("pool lease scheduler requires a valid worker");
    }
    impl_ = std::make_unique<Impl>(poolSize, &worker, resource);
}

PoolLeaseScheduler::~PoolLeaseScheduler() = default;

Task<PoolWaiterResult> PoolLeaseScheduler::acquire(
    std::optional<std::chrono::milliseconds> timeout) {
    return Impl::acquireReserved(Impl::AcquireReservation(*impl_), timeout, {}, &impl_->worker_);
}

Task<PoolWaiterResult> PoolLeaseScheduler::acquire(
    std::optional<std::chrono::milliseconds> timeout, StopToken stopToken) {
    return Impl::acquireReserved(
        Impl::AcquireReservation(*impl_), timeout, std::move(stopToken), &impl_->worker_);
}

PoolLeaseReleaseStatus PoolLeaseScheduler::release(std::size_t slot) noexcept {
    return impl_->release(slot);
}

bool PoolLeaseScheduler::close() noexcept {
    return impl_->close();
}

void PoolLeaseScheduler::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    impl_->scanDeadlines(now);
}

bool PoolLeaseScheduler::closing() const noexcept {
    return impl_->closing_;
}

}  // namespace ruvia
