#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/detail/redis/RedisArgumentPack.h"
#include "ruvia/web/detail/redis/RedisBatchForwarding.h"
#include "ruvia/web/detail/redis/RedisCommandBatch.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia {

class RedisTransaction final {
public:
    RedisTransaction(const RedisTransaction&) = delete;
    RedisTransaction& operator=(const RedisTransaction&) = delete;
    RedisTransaction(RedisTransaction&& other) noexcept;
    RedisTransaction& operator=(RedisTransaction&&) = delete;

    RUVIA_REDIS_BATCH_FORWARDING(RedisTransaction)
    RedisTransaction& command(std::span<const std::string_view> args);
    RedisTransaction& command(std::initializer_list<std::string_view> args) = delete;
    RedisTransaction& watch(std::string_view key);
    RedisTransaction& watch(std::span<const std::string_view> keys);

    // As on RedisPipeline: command words and watched keys as ordinary arguments,
    // copied into the batch before the call returns. Two or more keys, so a
    // single-key watch(key) keeps its own overload.
    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    RedisTransaction& command(Args&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    template <typename... Keys>
        requires(detail::RedisArgumentPack<Keys...> && sizeof...(Keys) >= 2)
    RedisTransaction& watch(Keys&&... keys) {
        const std::string_view views[]{std::string_view(keys)...};
        return watch(std::span<const std::string_view>(views));
    }

    RedisTransaction& unwatch();

    // A transaction is a single-use command batch. Its commands are transferred
    // into the returned coroutine frame before this builder may be destroyed.
    ScopedOperation<std::pmr::vector<RedisValue>> exec() &&;

private:
    friend class RedisHandle;

    RedisTransaction(detail::RedisPool& pool, OperationOptions options,
        std::pmr::memory_resource* resource, operation_scope& scope) noexcept;
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> execute_owned(
        detail::redis_command_payload payload, std::pmr::vector<detail::redis_owned_command> watches);

    detail::redis_command_batch batch_;
    std::pmr::vector<detail::redis_owned_command> watches_;
    scoped_capability_registration registration_;
    static void expire_capability(void* target) noexcept;
};

}  // namespace ruvia
