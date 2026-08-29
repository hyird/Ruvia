#include "ruvia/web/detail/redis/RedisRegistry.h"
#include <hiredis/hiredis.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia::detail {
namespace {

[[nodiscard]] const WorkerHandle& requireRedisWorker(const WorkerHandle& worker) {
    if (!worker.valid()) {
        throw std::invalid_argument("redis pool requires a valid worker");
    }
    return worker;
}

}  // namespace

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
    const WorkerHandle& worker, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      worker_(requireRedisWorker(worker)),
      config_(config),
      commandTimeout_(commandTimeout),
      resource_(detail::pmrResourceOrDefault(resource)),
      connections_(resource_),
      scheduler_(poolSize, worker_, resource_),
      cancellationMailbox_(makeWorkerCancellationMailbox(*this, worker_)) {
    connections_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
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
    cancellationMailbox_->detach(*this);
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

}  // namespace ruvia::detail
