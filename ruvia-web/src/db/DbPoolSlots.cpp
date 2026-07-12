#include "ruvia/web/detail/db/DbInternal.h"

#include <exception>
#include <stdexcept>

namespace ruvia {

detail::MariaDbPool::SlotGuard::SlotGuard(MariaDbPool& client, std::size_t slot) noexcept
    : client_(&client),
      slot_(slot) {}

detail::MariaDbPool::SlotGuard::~SlotGuard() {
    if (client_ != nullptr) {
        client_->releaseSlot(slot_);
    }
}

Task<std::size_t> detail::MariaDbPool::acquireSlot() {
    if (closing_) {
        throw std::runtime_error("database client is closing");
    }
    if (!freeSlots_.empty()) {
        const auto slot = freeSlots_.back();
        freeSlots_.pop_back();
        slots_[slot].busy = true;
        co_return slot;
    }

    struct WaiterGuard final {
        MariaDbPool& client;
        PoolWaiter& waiter;

        ~WaiterGuard() {
            client.waiters_.remove(waiter);
        }
    };

    PoolWaiter waiter(
        std::chrono::steady_clock::now() + config_.acquireTimeout);
    waiters_.enqueue(waiter);
    WaiterGuard guard{*this, waiter};

    const auto& result = co_await waiter;
    if (result.timedOut() != nullptr) {
        throw std::runtime_error("database connection pool acquire timed out");
    }
    if (result.closed() != nullptr) {
        throw std::runtime_error("database client is closing");
    }

    const auto* acquired = result.acquired();
    if (acquired == nullptr) {
        std::terminate();
    }
    co_return acquired->index();
}

void detail::MariaDbPool::releaseSlot(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    if (!closing_ && waiters_.resumeNext(slot)) {
        return;
    }

    if (slots_[slot].busy) {
        slots_[slot].busy = false;
        freeSlots_.push_back(slot);
    }
}

}  // namespace ruvia
