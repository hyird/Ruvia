#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include "HttpClientPool.h"

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
    HttpClientPool::PoolWaiter waiter;
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
        waiter.ready = &ready;
        waiter.timedOut = &timedOut;
        waiter.index = &index;
        waiter.deadline = std::chrono::steady_clock::now() + pool.config_.acquireTimeout;
        waiter.handle = handle;
        pool.enqueueWaiter(waiter);
        return true;
    }

    std::size_t await_resume() {
        pool.removeWaiter(waiter);
        if (timedOut) {
            throw std::runtime_error("http client pool acquire timed out");
        }
        if (pool.closing_) {
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
    if (!resumeNextWaiter(index)) {
        free_.push_back(index);
    }
}

void HttpClientPool::enqueueWaiter(PoolWaiter& w) noexcept {
    w.previous = waiterTail_;
    w.next = nullptr;
    if (waiterTail_ != nullptr) {
        waiterTail_->next = &w;
    } else {
        waiterHead_ = &w;
    }
    waiterTail_ = &w;
    w.queued = true;
}

void HttpClientPool::removeWaiter(PoolWaiter& w) noexcept {
    if (!w.queued) {
        return;
    }
    if (w.previous != nullptr) {
        w.previous->next = w.next;
    } else {
        waiterHead_ = w.next;
    }
    if (w.next != nullptr) {
        w.next->previous = w.previous;
    } else {
        waiterTail_ = w.previous;
    }
    w.queued = false;
}

bool HttpClientPool::resumeNextWaiter(std::size_t index) noexcept {
    if (waiterHead_ == nullptr) {
        return false;
    }
    auto* w = waiterHead_;
    removeWaiter(*w);
    *w->index = index;
    *w->ready = true;
    if (w->handle) {
        w->handle.resume();
    }
    return true;
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
