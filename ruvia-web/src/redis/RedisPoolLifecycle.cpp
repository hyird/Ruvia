#include "ruvia/web/detail/redis/RedisRegistry.h"
#include <hiredis/hiredis.h>

#include <system_error>
#include <utility>

namespace ruvia::detail {

void RedisReaderDeleter::operator()(redisReader* reader) const noexcept {
    if (reader != nullptr) {
        redisReaderFree(reader);
    }
}

RedisPool::Connection::Connection(asio::io_context& ioContext, std::pmr::memory_resource* resource)
    : socket(ioContext),
      resolver(ioContext),
      writeBuffer(detail::pmrResourceOrDefault(resource)),
      reader(redisReaderCreate()),
      deadlineTimer(makePmrObject<WorkerTimerRegistration>(resource)) {}

RedisPool::Connection::~Connection() = default;

RedisPool::Connection::Connection(Connection&&) noexcept = default;
RedisPool::Connection& RedisPool::Connection::operator=(Connection&&) noexcept = default;

RedisPool::RedisPool(asio::io_context& ioContext, const RedisConfigStorage& config,
    std::optional<std::chrono::milliseconds> commandTimeout, std::size_t poolSize,
    std::pmr::memory_resource* resource, const WorkerHandle* worker)
    : ioContext_(ioContext),
      worker_(worker),
      config_(config),
      commandTimeout_(commandTimeout),
      resource_(detail::pmrResourceOrDefault(resource)),
      connections_(resource_),
      scheduler_(poolSize, resource_) {
    connections_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
    }
    if (worker_ != nullptr) {
        cancellationMailbox_ = makeWorkerCancellationMailbox(*this, *worker_);
    }
}

RedisPool::~RedisPool() {
    closeNow();
}

Task<void> RedisPool::connect() {
    for (auto& connection : connections_) {
        if (!connection.connected) {
            co_await connect(connection);
        }
    }
    co_return;
}

void RedisPool::closeNow() noexcept {
    if (cancellationMailbox_ != nullptr) {
        cancellationMailbox_->detach(*this);
    }
    if (!scheduler_.close()) {
        return;
    }
    for (auto& connection : connections_) {
        if (connection.abortReason == Connection::AbortReason::kNone) {
            connection.abortReason = Connection::AbortReason::kClosing;
        }
        close(connection);
    }
}

void RedisPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    scheduler_.scanDeadlines(now);

    for (auto& connection : connections_) {
        const auto kind = connection.deadline.expire(now);
        if (!kind.has_value()) {
            continue;
        }
        std::error_code ignored;
        connection.deadlineTimer->cancel();
        if (*kind == Connection::DeadlineKind::kResolve) {
            connection.resolver.cancel();
        } else if (*kind == Connection::DeadlineKind::kSocket) {
            connection.socket.cancel(ignored);
        }
    }
}

bool RedisPool::needsDeadlineScan() const noexcept {
    // Production pools have a WorkerHandle and arm exact timers for both
    // configured and per-operation deadlines. A standalone pool without a
    // worker retains the explicit scanDeadlines() fallback.
    return worker_ == nullptr;
}

}  // namespace ruvia::detail
