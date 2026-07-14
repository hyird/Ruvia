#include "ruvia/web/detail/db/DbPoolScheduler.h"

#include <exception>
#include <stdexcept>

namespace ruvia::detail {

DbPoolScheduler::DbPoolScheduler(
    std::size_t poolSize,
    std::pmr::memory_resource* resource)
    : freeSlots_(pmrResourceOrDefault(resource)),
      busy_(pmrResourceOrDefault(resource)) {
    freeSlots_.reserve(poolSize);
    busy_.resize(poolSize, 0);
    for (std::size_t i = 0; i < poolSize; ++i) {
        freeSlots_.push_back(i);
    }
}

Task<std::size_t> DbPoolScheduler::acquire(
    std::optional<std::chrono::milliseconds> timeout) {
    if (closing_) {
        throw std::runtime_error("database client is closing");
    }
    if (!freeSlots_.empty()) {
        const auto slot = freeSlots_.back();
        freeSlots_.pop_back();
        busy_[slot] = 1;
        co_return slot;
    }

    struct WaiterGuard final {
        PoolWaiterQueue& queue;
        PoolWaiter& waiter;

        ~WaiterGuard() {
            queue.remove(waiter);
        }
    };

    const auto deadline = timeout.has_value()
        ? std::chrono::steady_clock::now() + *timeout
        : std::chrono::steady_clock::time_point::max();
    PoolWaiter waiter(deadline);
    waiters_.enqueue(waiter);
    WaiterGuard guard{waiters_, waiter};

    const auto& result = co_await waiter;
    if (result.timedOut() != nullptr) {
        throw std::runtime_error("database connection pool acquire timed out");
    }
    if (result.closed() != nullptr) {
        throw std::runtime_error("database client is closing");
    }

    const auto* acquired = result.acquired();
    if (acquired == nullptr || acquired->index() >= busy_.size()) {
        std::terminate();
    }
    busy_[acquired->index()] = 1;
    co_return acquired->index();
}

void DbPoolScheduler::release(std::size_t slot) noexcept {
    if (slot >= busy_.size() || busy_[slot] == 0) {
        return;
    }
    if (!closing_ && waiters_.resumeNext(slot)) {
        return;
    }
    busy_[slot] = 0;
    freeSlots_.push_back(slot);
}

bool DbPoolScheduler::close() noexcept {
    if (closing_) {
        return false;
    }
    closing_ = true;
    waiters_.closeAll();
    return true;
}

void DbPoolScheduler::scanDeadlines(
    std::chrono::steady_clock::time_point now) noexcept {
    waiters_.expireDeadlines(now);
}

bool DbPoolScheduler::closing() const noexcept {
    return closing_;
}

}  // namespace ruvia::detail
