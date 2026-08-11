#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

RedisPipeline::RedisPipeline(detail::RedisPool& pool, OperationOptions options, std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope) noexcept
    : detail::ScopedCapabilityNode(operationScope, &RedisPipeline::expireCapability),
      state_(std::in_place_type<Ready>, pool),
      operationOptions_(std::move(options)),
      commands_(detail::pmrResourceOrDefault(resource)) {}

RedisPipeline::RedisPipeline(RedisPipeline&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)),
      operationOptions_(std::move(other.operationOptions_)),
      commands_(std::move(other.commands_)) {
    other.state_.template emplace<Consumed>();
}

void RedisPipeline::requireActive() const {
    detail::ScopedCapabilityNode::requireActive();
    if (!std::holds_alternative<Ready>(state_)) {
        throw std::logic_error("redis pipeline has already been consumed");
    }
}

void RedisPipeline::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& pipeline = static_cast<RedisPipeline&>(capability);
    pipeline.state_.template emplace<Consumed>();
    pipeline.operationOptions_ = {};
    std::pmr::vector<Command> empty(pipeline.commands_.get_allocator().resource());
    pipeline.commands_.swap(empty);
}

detail::RedisPool& RedisPipeline::consumePool() {
    auto* ready = std::get_if<Ready>(&state_);
    if (ready == nullptr) {
        throw std::logic_error("redis pipeline has already been consumed");
    }
    auto& pool = ready->pool.get();
    state_.template emplace<Consumed>();
    return pool;
}

std::pmr::memory_resource* RedisPipeline::resource() const noexcept {
    return commands_.get_allocator().resource();
}

detail::ScopedOperationScope& RedisPipeline::operationScope() const {
    return detail::ScopedCapabilityNode::operationScope();
}

RedisPipeline::Command RedisPipeline::makeCommand(std::pmr::memory_resource* resource, std::span<const std::string_view> args) {
    Command command{std::pmr::vector<std::pmr::string>(resource)};
    command.args.reserve(args.size());
    for (const auto arg : args) {
        detail::emplaceRedisString(command.args, arg);
    }
    return command;
}

RedisPipeline::Command RedisPipeline::makeCommand(std::pmr::memory_resource* resource, std::string_view first, std::span<const std::string_view> rest) {
    Command command{std::pmr::vector<std::pmr::string>(resource)};
    command.args.reserve(rest.size() + 1);
    detail::emplaceRedisString(command.args, first);
    for (const auto arg : rest) {
        detail::emplaceRedisString(command.args, arg);
    }
    return command;
}

void RedisPipeline::appendCommand(std::pmr::vector<Command>& target, std::pmr::memory_resource* resource, std::span<const std::string_view> args) {
    target.emplace_back(makeCommand(resource, args));
}

void RedisPipeline::appendCommand(std::pmr::vector<Command>& target, std::pmr::memory_resource* resource, std::string_view first, std::span<const std::string_view> rest) {
    target.emplace_back(makeCommand(resource, first, rest));
}

RedisPipeline& RedisPipeline::command(std::span<const std::string_view> args) {
    requireActive();
    (void)detail::validateRedisPooledCommand(args, false);
    appendCommand(commands_, resource(), args);
    return *this;
}


Task<std::pmr::vector<RedisValue>> RedisPipeline::executeOwned(detail::RedisPool& pool, OperationOptions options, std::pmr::vector<Command> commands, std::pmr::memory_resource* resource) {
    co_return co_await pool.executePipeline(std::span<const Command>(commands), std::move(options), resource);
}

ScopedOperation<std::pmr::vector<RedisValue>> RedisPipeline::exec() && {
    requireActive();
    auto* commandResource = resource();
    auto& pool = consumePool();
    return detail::makeScopedOperation(
        operationScope(),
        executeOwned(pool, operationOptions_, std::move(commands_), commandResource));
}

}  // namespace ruvia
