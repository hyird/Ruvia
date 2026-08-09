#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/web/redis/RedisTypes.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/redis/RedisArgumentPack.h"
#include "ruvia/web/detail/redis/RedisCommandBatchMixin.h"

#include <functional>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ruvia {

class RedisHandle;
class RedisTransaction;

class RedisPipeline final : private detail::ScopedCapabilityNode, public detail::RedisCommandBatchMixin<RedisPipeline> {
public:
    RedisPipeline(const RedisPipeline&) = delete;
    RedisPipeline& operator=(const RedisPipeline&) = delete;
    RedisPipeline(RedisPipeline&& other) noexcept;
    RedisPipeline& operator=(RedisPipeline&&) = delete;

    RedisPipeline& command(std::span<const std::string_view> args);
    RedisPipeline& command(std::initializer_list<std::string_view> args) = delete;

    // Command words as ordinary arguments: command("TYPE", key). Each queued
    // command owns its arguments (makeCommand copies them into the batch before
    // returning), so the temporary array does not outlive this call.
    template <typename... Args>
        requires detail::RedisArgumentPack<Args...>
    RedisPipeline& command(Args&&... args) {
        const std::string_view views[]{std::string_view(args)...};
        return command(std::span<const std::string_view>(views));
    }

    // Typed command words (get/set/hget/...) are shared with RedisTransaction
    // through detail::RedisCommandBatchMixin.

    // Consumes the batch before returning the lazy Task, so the coroutine frame
    // owns every command and never borrows this builder through `this`.
    ScopedOperation<std::pmr::vector<RedisValue>> exec() &&;
    ScopedOperation<std::pmr::vector<RedisValue>> exec(RedisOperationOptions options) &&;

private:
    friend class RedisHandle;
    friend class RedisTransaction;
    friend class detail::RedisPool;
    friend class detail::RedisCommandBatchMixin<RedisPipeline>;

    struct Command final {
        std::pmr::vector<std::pmr::string> args;
    };

    [[nodiscard]] static Command makeCommand(std::pmr::memory_resource* resource, std::span<const std::string_view> args);
    [[nodiscard]] static Command makeCommand(std::pmr::memory_resource* resource, std::string_view first, std::span<const std::string_view> rest = {});
    static void appendCommand(std::pmr::vector<Command>& target, std::pmr::memory_resource* resource, std::span<const std::string_view> args);
    static void appendCommand(std::pmr::vector<Command>& target, std::pmr::memory_resource* resource, std::string_view first, std::span<const std::string_view> rest = {});

    RedisPipeline(detail::RedisPool& pool, RedisOperationOptions options, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope) noexcept;
    [[nodiscard]] static Task<std::pmr::vector<RedisValue>> executeOwned(detail::RedisPool& pool, RedisOperationOptions options, std::pmr::vector<Command> commands, std::pmr::memory_resource* resource);
    void requireActive() const;
    [[nodiscard]] detail::RedisPool& consumePool();
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;
    [[nodiscard]] detail::ScopedOperationScope& operationScope() const;

    struct Ready final {
        explicit Ready(detail::RedisPool& owner) noexcept
            : pool(owner) {}

        std::reference_wrapper<detail::RedisPool> pool;
    };

    struct Consumed final {};

    std::variant<Ready, Consumed> state_;
    RedisOperationOptions operationOptions_;
    std::pmr::vector<Command> commands_;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
};

}  // namespace ruvia
