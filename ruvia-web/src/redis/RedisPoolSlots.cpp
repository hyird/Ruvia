#include "ruvia/web/detail/redis/RedisInternal.h"

#include <exception>

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
    const auto result = co_await scheduler_.acquire(config_.acquireTimeout);
    if (result.timedOut() != nullptr) {
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }
    if (result.closed() != nullptr) {
        throw RedisError(RedisError::Code::kIoError, "redis pool is closing");
    }

    const auto* acquired = result.acquired();
    if (acquired == nullptr) {
        std::terminate();
    }
    co_return acquired->index();
}

void RedisPool::release(std::size_t index) noexcept {
    scheduler_.release(index);
}

}  // namespace ruvia::detail
