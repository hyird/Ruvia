#include "ruvia/web/redis/RedisPipeline.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

namespace ruvia::detail {

redis_command_batch::redis_command_batch(RedisPool& pool, OperationOptions options,
    std::pmr::memory_resource* resource) noexcept
    : pool_(&pool),
      options_(std::move(options)),
      commands_(pmrResourceOrDefault(resource)) {}

redis_command_batch::redis_command_batch(redis_command_batch&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      options_(std::move(other.options_)),
      commands_(std::move(other.commands_)) {}

void redis_command_batch::require_ready() const {
    if (pool_ == nullptr) {
        throw std::logic_error("redis command batch has already been consumed or expired");
    }
}

std::pmr::memory_resource* redis_command_batch::resource() const noexcept {
    return commands_.get_allocator().resource();
}

redis_command_payload redis_command_batch::consume() {
    require_ready();
    auto& pool = *std::exchange(pool_, nullptr);
    return {pool, std::move(options_), std::move(commands_)};
}

void redis_command_batch::expire() noexcept {
    pool_ = nullptr;
    options_ = {};
    std::pmr::vector<redis_owned_command> empty(resource());
    commands_.swap(empty);
}

redis_command_batch& redis_command_batch::command(std::span<const std::string_view> args) {
    require_ready();
    (void)validateRedisPooledCommand(args, false);
    commands_.emplace_back(make_owned_redis_command(resource(), args));
    return *this;
}

}  // namespace ruvia::detail

namespace ruvia {

RedisPipeline::RedisPipeline(detail::RedisPool& pool, OperationOptions options,
    std::pmr::memory_resource* resource, operation_scope& operationScope) noexcept
    : batch_(pool, std::move(options), resource),
      registration_(operationScope, this, &RedisPipeline::expire_capability) {}

RedisPipeline::RedisPipeline(RedisPipeline&& other) noexcept
    : batch_(std::move(other.batch_)),
      registration_(std::move(other.registration_), this) {}

void RedisPipeline::expire_capability(void* target) noexcept {
    static_cast<RedisPipeline*>(target)->batch_.expire();
}

RedisPipeline& RedisPipeline::command(std::span<const std::string_view> args) {
    registration_.require_active();
    batch_.command(args);
    return *this;
}

Task<std::pmr::vector<RedisValue>> RedisPipeline::execute_owned(
    detail::redis_command_payload payload) {
    co_return co_await payload.pool.get().executePipeline(
        std::span<const detail::redis_owned_command>(payload.commands),
        std::move(payload.options), payload.commands.get_allocator().resource());
}

ScopedOperation<std::pmr::vector<RedisValue>> RedisPipeline::exec() && {
    auto& scope = registration_.scope();
    auto payload = batch_.consume();
    return make_scoped_operation(scope, execute_owned(std::move(payload)));
}

}  // namespace ruvia
