#include "../DbInternal.h"

#include <coroutine>
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

    bool ready = false;
    bool timedOut = false;
    std::size_t slot = 0;
    PoolWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .index = &slot,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    waiters_.enqueue(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        PoolWaiter& waiter;
        bool& ready;

        [[nodiscard]] bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter.handle = handle;
        }

        void await_resume() const noexcept {}
    };

    co_await WaiterAwaiter{waiter, ready};

    if (timedOut) {
        throw std::runtime_error("database connection pool acquire timed out");
    }

    if (closing_ || slot >= slots_.size()) {
        throw std::runtime_error("database client is closing");
    }

    co_return slot;
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
