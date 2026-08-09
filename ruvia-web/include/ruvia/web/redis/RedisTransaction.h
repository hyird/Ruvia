#pragma once

#include "ruvia/web/redis/RedisPipeline.h"
#include "ruvia/web/detail/redis/RedisArgumentPack.h"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace ruvia {

class RedisTransaction final : private detail::ScopedCapabilityNode, public detail::RedisCommandBatchMixin<RedisTransaction> {
public:
    RedisTransaction(const RedisTransaction&) = delete;
    RedisTransaction& operator=(const RedisTransaction&) = delete;
    RedisTransaction(RedisTransaction&&) noexcept = default;
    RedisTransaction& operator=(RedisTransaction&&) = delete;

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
    // Typed command words (get/set/hget/...) are shared with RedisPipeline
    // through detail::RedisCommandBatchMixin.

    // A transaction is a single-use command batch. Its commands are transferred
    // into the returned coroutine frame before this builder may be destroyed.
    ScopedOperation<std::pmr::vector<RedisValue>> exec() &&;
    ScopedOperation<std::pmr::vector<RedisValue>> exec(RedisOperationOptions options) &&;

private:
    friend class RedisHandle;
    friend class detail::RedisCommandBatchMixin<RedisTransaction>;

    explicit RedisTransaction(RedisPipeline pipeline) noexcept;
    void requireActive() const { pipeline_.requireActive(); }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return pipeline_.resource(); }
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> executeOwned(detail::RedisPool& pool, RedisOperationOptions options, std::pmr::memory_resource* resource, std::pmr::vector<RedisPipeline::Command> watches, std::pmr::vector<RedisPipeline::Command> commands);

    RedisPipeline pipeline_;
    std::pmr::vector<RedisPipeline::Command> watches_;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
};

}  // namespace ruvia
