#include "../RedisInternal.h"

#include <coroutine>

namespace ruvia::detail {

RedisPool::ConnectionGuard::ConnectionGuard(RedisPool& pool, std::size_t index) noexcept
    : pool_(&pool),
      index_(index) {}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    if (pool_ == nullptr) {
        return;
    }
    if (discard_) {
        pool_->close(pool_->connections_[index_]);
    }
    pool_->release(index_);
}

RedisPool::Connection& RedisPool::ConnectionGuard::connection() noexcept {
    return pool_->connections_[index_];
}

void RedisPool::ConnectionGuard::discard() noexcept {
    discard_ = true;
}

Task<std::size_t> RedisPool::acquire() {
    if (closing_) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }
    if (!free_.empty()) {
        const auto index = free_.back();
        free_.pop_back();
        connections_[index].busy = true;
        co_return index;
    }

    struct WaiterGuard final {
        RedisPool& pool;
        PoolWaiter& waiter;

        ~WaiterGuard() {
            pool.removeWaiter(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t waitedIndex = 0;
    PoolWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .index = &waitedIndex,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    enqueueWaiter(waiter);
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
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }

    if (closing_ || waitedIndex >= connections_.size()) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }

    co_return waitedIndex;
}

void RedisPool::release(std::size_t index) noexcept {
    if (index >= connections_.size()) {
        return;
    }
    if (closing_) {
        connections_[index].busy = false;
        return;
    }
    if (resumeNextWaiter(index)) {
        connections_[index].busy = true;
        return;
    }
    connections_[index].busy = false;
    free_.push_back(index);
}

void RedisPool::enqueueWaiter(PoolWaiter& waiter) noexcept {
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

void RedisPool::removeWaiter(PoolWaiter& waiter) noexcept {
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

bool RedisPool::resumeNextWaiter(std::size_t index) noexcept {
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr && waiter->index != nullptr) {
            *waiter->index = index;
            *waiter->ready = true;
            if (waiter->handle) {
                waiter->handle.resume();
            }
            return true;
        }
    }
    return false;
}

}  // namespace ruvia::detail
