#include "ruvia/web/detail/redis/RedisRegistry.h"

#include <exception>
#include <stdexcept>

namespace ruvia::detail {

RedisPool::ConnectionGuard::ConnectionGuard(RedisPool& pool, std::size_t index) noexcept
    : pool_(pool),
      index_(index) {}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    if (discard_) {
        pool_.close(pool_.connections_[index_]);
    }
    pool_.release(index_);
}

RedisPool::Connection& RedisPool::ConnectionGuard::connection() noexcept {
    return pool_.connections_[index_];
}

void RedisPool::ConnectionGuard::discard() noexcept {
    discard_ = true;
}

Task<std::size_t> RedisPool::acquire(const OperationTimeout& timeout, StopToken stopToken) {
    if (stopToken.stoppable() && (worker_ == nullptr || !worker_->valid())) {
        throw std::logic_error("cancellable redis operation requires a valid worker");
    }
    PoolWaiterResult result = worker_ != nullptr
        ? co_await scheduler_.acquire(timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken), *worker_)
        : co_await scheduler_.acquire(timeout.constrainedBy(config_.acquireTimeout).remaining());
    if (result.timedOut() != nullptr) {
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw RedisError(RedisError::Code::kCancelled, "redis operation cancelled");
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
    const auto status = scheduler_.release(index);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot || status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

}  // namespace ruvia::detail
