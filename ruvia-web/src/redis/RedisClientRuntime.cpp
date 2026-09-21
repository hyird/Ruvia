#include "ruvia/web/detail/redis/RedisClientRuntime.h"

#include <utility>

#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia::detail {

RedisClientRuntime::RedisClientRuntime(asio::io_context& ioContext, const WorkerHandle& worker,
    RedisConfigStorage config, std::pmr::memory_resource* resource)
    : config_(std::move(config)),
      resource_(resource),
      general_(makePmrObject<RedisPool>(resource_, ioContext, config_,
          config_.commandTimeout, config_.poolSizePerWorker, worker, resource_)),
      // Blocking waits have their own deadlines; ordinary command timeouts
      // must not interrupt a finite long wait or an explicitly cancelled wait.
      blocking_(makePmrObject<RedisPool>(resource_, ioContext, config_,
          std::nullopt, config_.blockingPoolSizePerWorker, worker, resource_)) {}

RedisClientRuntime::~RedisClientRuntime() = default;

Task<void> RedisClientRuntime::connect() {
    // Validate ordinary connections at startup; blocking slots connect lazily.
    co_await general_->connect();
}

void RedisClientRuntime::closeNow() noexcept {
    general_->closeNow();
    blocking_->closeNow();
}

RedisHandle RedisClientRuntime::handle(ScopedOperationScope& scope) const {
    return RedisHandle(*general_, *blocking_, resource_, scope);
}

}  // namespace ruvia::detail
