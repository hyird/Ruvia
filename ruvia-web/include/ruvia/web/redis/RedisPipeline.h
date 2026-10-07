#pragma once

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/detail/redis/RedisBatchForwarding.h"
#include "ruvia/web/detail/redis/RedisCommandBatch.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia {

class RedisHandle;

class RedisPipeline final {
public:
    RedisPipeline(const RedisPipeline&) = delete;
    RedisPipeline& operator=(const RedisPipeline&) = delete;
    RedisPipeline(RedisPipeline&& other) noexcept;
    RedisPipeline& operator=(RedisPipeline&&) = delete;

    RUVIA_REDIS_BATCH_FORWARDING(RedisPipeline)

    RedisPipeline& command(std::span<const std::string_view> args);
    RedisPipeline& command(std::initializer_list<std::string_view> args) = delete;

    // Every command owns its arguments before this call returns.
    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    RedisPipeline& command(Args&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    // Consumes the batch before returning the lazy Task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    ScopedOperation<std::pmr::vector<RedisValue>> exec() &&;

private:
    friend class RedisHandle;
    detail::redis_command_batch batch_;
    scoped_capability_registration registration_;

    RedisPipeline(detail::RedisPool& pool, OperationOptions options,
        std::pmr::memory_resource* resource, operation_scope& operationScope) noexcept;
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> execute_owned(
        detail::redis_command_payload payload);

    static void expire_capability(void* target) noexcept;
};

}  // namespace ruvia
