#include "ruvia/web/detail/redis/RedisRegistry.h"

#include <exception>

namespace ruvia::detail {

RedisPool::ConnectionGuard::ConnectionGuard(
    RedisPool& pool, std::size_t index, const StopToken& stopToken)
    : pool_(pool),
      index_(index) {
    auto& connection = pool_.connections_[index_];
    connection.abortReason = Connection::AbortReason::kNone;
    connection.cancellationId = 0;
    if (!stopToken.stoppable()) {
        return;
    }
    if (pool_.cancellationMailbox_ == nullptr) {
        std::terminate();
    }
    cancellationId_ = pool_.cancellationMailbox_->nextOperationId();
    connection.cancellationId = cancellationId_;
    try {
        stopToken.registerCallback(stopRegistration_,
            WorkerCancellationPost<RedisOperationCancellationMailbox>(
                pool_.cancellationMailbox_, cancellationId_));
    } catch (...) {
        connection.cancellationId = 0;
        throw;
    }
    if (stopToken.stopRequested()) {
        pool_.cancelOperationById(cancellationId_);
    }
}

RedisPool::ConnectionGuard::~ConnectionGuard() {
    stopRegistration_.reset();
    auto& connection = pool_.connections_[index_];
    if (connection.cancellationId == cancellationId_) {
        connection.cancellationId = 0;
    }
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

Task<std::size_t> RedisPool::acquire(const OperationTimeout& timeout, StopToken stopToken) {
    const auto result = co_await scheduler_.acquire(
        timeout.constrainedBy(config_.acquireTimeout).remaining(), std::move(stopToken), worker_);
    if (result.timedOut() != nullptr) {
        throw RedisError(RedisError::Code::kTimeout, "redis connection pool acquire timed out");
    }
    if (result.cancelled() != nullptr) {
        throw RedisError(RedisError::Code::kCancelled, "redis operation cancelled");
    }
    if (result.closed() != nullptr) {
        throw RedisError(RedisError::Code::kClosing, "redis pool is closing");
    }

    const auto* acquired = result.acquired();
    if (acquired == nullptr) {
        std::terminate();
    }
    co_return acquired->index();
}

void RedisPool::release(std::size_t index) noexcept {
    const auto status = scheduler_.release(index);
    if (status == PoolLeaseReleaseStatus::kInvalidSlot ||
        status == PoolLeaseReleaseStatus::kAlreadyReleased) {
        std::terminate();
    }
}

}  // namespace ruvia::detail
