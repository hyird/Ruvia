#include "ruvia/web/detail/redis/RedisInternal.h"

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
            pool.waiters_.remove(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t waitedIndex = 0;
    PoolWaiter waiter(
        ready,
        timedOut,
        waitedIndex,
        std::chrono::steady_clock::now() + config_.acquireTimeout);
    waiters_.enqueue(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        PoolWaiter& waiter;
        bool& ready;

        [[nodiscard]] bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter.setHandle(handle);
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
    if (waiters_.resumeNext(index)) {
        connections_[index].busy = true;
        return;
    }
    connections_[index].busy = false;
    free_.push_back(index);
}

}  // namespace ruvia::detail
