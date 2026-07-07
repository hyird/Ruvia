#include "../RedisInternal.h"
#include "RedisConfigValidation.h"
#include "RedisUtils.h"

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
      reader(redisReaderCreate()) {}

RedisPool::Connection::~Connection() = default;

RedisPool::Connection::Connection(Connection&&) noexcept = default;
RedisPool::Connection& RedisPool::Connection::operator=(Connection&&) noexcept = default;

RedisPool::RedisPool(asio::io_context& ioContext, RedisConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(detail::pmrResourceOrDefault(resource)),
      connections_(resource_),
      free_(resource_) {
    validateRedisConfig(config_);
    const auto poolSize = config_.poolSizePerWorker;
    connections_.reserve(poolSize);
    free_.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; ++i) {
        connections_.emplace_back(ioContext_, resource_);
        free_.push_back(i);
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
    closing_ = true;
    waiters_.closeAll(connections_.size());
    for (auto& connection : connections_) {
        close(connection);
    }
}

void RedisPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    if (config_.acquireTimeout.count() > 0) {
        waiters_.expireDeadlines(now);
    }

    for (auto& connection : connections_) {
        if (!connection.deadlineActive || connection.deadline > now) {
            continue;
        }
        connection.timedOut = true;
        std::error_code ignored;
        if (connection.deadlineKind == Connection::DeadlineKind::kResolve) {
            connection.resolver.cancel();
        } else if (connection.deadlineKind == Connection::DeadlineKind::kSocket) {
            connection.socket.cancel(ignored);
        }
    }
}

bool RedisPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
        config_.commandTimeout.count() > 0 ||
        config_.acquireTimeout.count() > 0;
}

}  // namespace ruvia::detail
