
#include "ruvia/web/detail/client/HttpClientPool.h"

#include <coroutine>
#include <stdexcept>

namespace ruvia::detail {

HttpClientPool::ConnectionGuard::ConnectionGuard(HttpClientPool& pool, std::size_t index) noexcept
    : pool_(&pool), index_(index) {}

HttpClientPool::ConnectionGuard::~ConnectionGuard() {
    if (pool_ == nullptr) {
        return;
    }
    if (discard_) {
        pool_->closeConnection(*pool_->connections_[index_]);
    }
    pool_->release(index_);
}

HttpClientPool::Connection& HttpClientPool::ConnectionGuard::connection() noexcept {
    return *pool_->connections_[index_];
}

struct PoolWaiterAwaiter {
    explicit PoolWaiterAwaiter(HttpClientPool& p) noexcept : pool(p) {}

    HttpClientPool& pool;
    PoolWaiter waiter;
    bool ready{false};
    bool timedOut{false};
    std::size_t index{0};

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!pool.free_.empty()) {
            index = pool.free_.back();
            pool.free_.pop_back();
            ready = true;
            return false;
        }
        waiter.bind(
            ready,
            timedOut,
            index,
            std::chrono::steady_clock::now() + pool.config_.acquireTimeout,
            handle);
        pool.waiters_.enqueue(waiter);
        return true;
    }

    std::size_t await_resume() {
        pool.waiters_.remove(waiter);
        if (timedOut) {
            throw std::runtime_error("http client pool acquire timed out");
        }
        if (pool.closing_) {
            // We may hold a real slot (popped in await_suspend, or handed to us by
            // release()->resumeNext) that we are about to abandon. Return it to free_
            // before throwing, or free_.size() stays below connections_.size() forever
            // and the retired pool never becomes quiescent (never gets reaped).
            if (ready) {
                pool.free_.push_back(index);
            }
            throw std::runtime_error("http client pool is closed");
        }
        return index;
    }
};

Task<std::size_t> HttpClientPool::acquire() {
    PoolWaiterAwaiter awaiter(*this);
    co_return co_await awaiter;
}

void HttpClientPool::release(std::size_t index) noexcept {
    if (!waiters_.resumeNext(index)) {
        free_.push_back(index);
    }
}

}  // namespace ruvia::detail
