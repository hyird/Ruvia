#pragma once

#include <memory>
#include <memory_resource>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/redis/RedisConfigStorage.h"
#include "ruvia/web/redis/RedisHandle.h"

namespace asio {
class io_context;
}

namespace ruvia::detail {

// The single backend owner used by standalone clients and App registrations.
// Both pools borrow this stable owner's normalized configuration and worker.
class RedisClientRuntime final {
public:
    RedisClientRuntime(asio::io_context& ioContext, const WorkerHandle& worker,
        RedisConfigStorage config, std::pmr::memory_resource* resource);
    RedisClientRuntime(asio::io_context&, WorkerHandle&&, RedisConfigStorage,
        std::pmr::memory_resource*) = delete;
    ~RedisClientRuntime();

    RedisClientRuntime(const RedisClientRuntime&) = delete;
    RedisClientRuntime& operator=(const RedisClientRuntime&) = delete;

    [[nodiscard]] Task<void> connect();
    void closeNow() noexcept;
    [[nodiscard]] RedisHandle handle(ScopedOperationScope& scope) const;

private:
    RedisConfigStorage config_;
    std::pmr::memory_resource* resource_;
    std::unique_ptr<RedisPool, PmrObjectDeleter<RedisPool>> general_;
    std::unique_ptr<RedisPool, PmrObjectDeleter<RedisPool>> blocking_;
};

}  // namespace ruvia::detail
