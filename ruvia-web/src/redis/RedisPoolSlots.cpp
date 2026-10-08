#include <exception>

#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia::detail {

RedisPool::ConnectionGuard::ConnectionGuard(
    RedisPool& pool, std::size_t index, const StopToken& stopToken)
    : pool_(pool),
      index_(index),
      cancellation_(pool.cancellation_target_, pool.connections_[index].cancellationId) {
    auto& connection = pool_.connections_[index_];
    connection.abortReason = Connection::AbortReason::kNone;
    cancellation_.arm(stopToken);
}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    cancellation_.reset();
    auto& connection = pool_.connections_[index_];
    if (discard_) {
        pool_.close(connection);
    }
    pool_.release(index_);
}

RedisPool::Connection& RedisPool::ConnectionGuard::connection() noexcept {
    return pool_.connections_[index_];
}

void RedisPool::ConnectionGuard::discard() noexcept {
    discard_ = true;
}

Task<std::size_t> RedisPool::acquire(const ruvia::OperationTimeout& timeout, StopToken stopToken) {
    const auto result = co_await scheduler_.acquire(
        timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken));
    switch (result.status()) {
        case PoolWaiterResult::Status::kAcquired:
            co_return result.index();
        case PoolWaiterResult::Status::kTimedOut:
            throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
        case PoolWaiterResult::Status::kCancelled:
            throw RedisError(RedisError::Code::kCancelled, "redis operation cancelled");
        case PoolWaiterResult::Status::kClosed:
            throw RedisError(RedisError::Code::kClosing, "redis pool is closing");
    }
    std::terminate();
}

void RedisPool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

}  // namespace ruvia::detail
