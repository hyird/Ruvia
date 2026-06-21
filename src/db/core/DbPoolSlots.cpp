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

void detail::MariaDbPool::enqueueWaiter(SlotWaiter& waiter) noexcept {
    if (waiter.queued) {
        return;
    }
    waiter.previous = waiterTail_;
    waiter.next = nullptr;
    waiter.queued = true;
    if (waiterTail_ != nullptr) {
        waiterTail_->next = &waiter;
    } else {
        waiterHead_ = &waiter;
    }
    waiterTail_ = &waiter;
}

void detail::MariaDbPool::removeWaiter(SlotWaiter& waiter) noexcept {
    if (!waiter.queued) {
        return;
    }
    if (waiter.previous != nullptr) {
        waiter.previous->next = waiter.next;
    } else {
        waiterHead_ = waiter.next;
    }
    if (waiter.next != nullptr) {
        waiter.next->previous = waiter.previous;
    } else {
        waiterTail_ = waiter.previous;
    }
    waiter.previous = nullptr;
    waiter.next = nullptr;
    waiter.queued = false;
}

bool detail::MariaDbPool::resumeNextWaiter(std::size_t slot) noexcept {
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr && waiter->slot != nullptr) {
            *waiter->slot = slot;
            *waiter->ready = true;
            if (waiter->handle) {
                waiter->handle.resume();
            }
            return true;
        }
    }
    return false;
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
        SlotWaiter& waiter;

        ~WaiterGuard() {
            client.removeWaiter(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t slot = 0;
    SlotWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .slot = &slot,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    enqueueWaiter(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        SlotWaiter& waiter;
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

    if (!closing_ && resumeNextWaiter(slot)) {
        return;
    }

    if (slots_[slot].busy) {
        slots_[slot].busy = false;
        freeSlots_.push_back(slot);
    }
}

}  // namespace ruvia
