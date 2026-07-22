#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

namespace {

template <typename... Args>
RedisPipeline& appendCommandArgs(RedisPipeline& pipeline, Args&&... args) {
    const std::string_view views[]{std::string_view(args)...};
    return pipeline.command(std::span<const std::string_view>(views, sizeof...(Args)));
}

}  // namespace

RedisPipeline::RedisPipeline(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource,
    detail::ScopedOperationScope& operationScope) noexcept
    : detail::ScopedCapabilityNode(operationScope, &RedisPipeline::expireCapability),
      state_(std::in_place_type<Ready>, pool),
      commands_(detail::pmrResourceOrDefault(resource)) {}

RedisPipeline::RedisPipeline(RedisPipeline&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)),
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

RedisPipeline::Command RedisPipeline::makeCommand(
    std::pmr::memory_resource* resource,
    std::span<const std::string_view> args) {
    Command command{std::pmr::vector<std::pmr::string>(resource)};
    command.args.reserve(args.size());
    for (const auto arg : args) {
        detail::emplaceRedisString(command.args, arg);
    }
    return command;
}

RedisPipeline::Command RedisPipeline::makeCommand(
    std::pmr::memory_resource* resource,
    std::string_view first,
    std::span<const std::string_view> rest) {
    Command command{std::pmr::vector<std::pmr::string>(resource)};
    command.args.reserve(rest.size() + 1);
    detail::emplaceRedisString(command.args, first);
    for (const auto arg : rest) {
        detail::emplaceRedisString(command.args, arg);
    }
    return command;
}

void RedisPipeline::appendCommand(
    std::pmr::vector<Command>& target,
    std::pmr::memory_resource* resource,
    std::span<const std::string_view> args) {
    target.emplace_back(makeCommand(resource, args));
}

void RedisPipeline::appendCommand(
    std::pmr::vector<Command>& target,
    std::pmr::memory_resource* resource,
    std::string_view first,
    std::span<const std::string_view> rest) {
    target.emplace_back(makeCommand(resource, first, rest));
}


RedisPipeline& RedisPipeline::command(std::span<const std::string_view> args) {
    requireActive();
    appendCommand(commands_, resource(), args);
    return *this;
}

RedisPipeline& RedisPipeline::get(std::string_view key) {
    return appendCommandArgs(*this, "GET", key);
}

RedisPipeline& RedisPipeline::set(std::string_view key, std::string_view value) {
    return appendCommandArgs(*this, "SET", key, value);
}

RedisPipeline& RedisPipeline::getDel(std::string_view key) {
    return appendCommandArgs(*this, "GETDEL", key);
}

RedisPipeline& RedisPipeline::getSet(std::string_view key, std::string_view value) {
    return appendCommandArgs(*this, "GETSET", key, value);
}

RedisPipeline& RedisPipeline::append(std::string_view key, std::string_view value) {
    return appendCommandArgs(*this, "APPEND", key, value);
}

RedisPipeline& RedisPipeline::strlen(std::string_view key) {
    return appendCommandArgs(*this, "STRLEN", key);
}

RedisPipeline& RedisPipeline::del(std::string_view key) {
    return appendCommandArgs(*this, "DEL", key);
}

RedisPipeline& RedisPipeline::unlink(std::string_view key) {
    return appendCommandArgs(*this, "UNLINK", key);
}

RedisPipeline& RedisPipeline::exists(std::string_view key) {
    return appendCommandArgs(*this, "EXISTS", key);
}

RedisPipeline& RedisPipeline::touch(std::string_view key) {
    return appendCommandArgs(*this, "TOUCH", key);
}

RedisPipeline& RedisPipeline::type(std::string_view key) {
    return appendCommandArgs(*this, "TYPE", key);
}

RedisPipeline& RedisPipeline::rename(std::string_view key, std::string_view newKey) {
    return appendCommandArgs(*this, "RENAME", key, newKey);
}

RedisPipeline& RedisPipeline::renameNx(std::string_view key, std::string_view newKey) {
    return appendCommandArgs(*this, "RENAMENX", key, newKey);
}

RedisPipeline& RedisPipeline::incr(std::string_view key) {
    return appendCommandArgs(*this, "INCR", key);
}

RedisPipeline& RedisPipeline::incrBy(std::string_view key, std::int64_t value) {
    requireActive();
    auto amount = detail::redisIntString(value, resource());
    return appendCommandArgs(*this, "INCRBY", key, std::string_view(amount));
}

RedisPipeline& RedisPipeline::decr(std::string_view key) {
    return appendCommandArgs(*this, "DECR", key);
}

RedisPipeline& RedisPipeline::decrBy(std::string_view key, std::int64_t value) {
    requireActive();
    auto amount = detail::redisIntString(value, resource());
    return appendCommandArgs(*this, "DECRBY", key, std::string_view(amount));
}

RedisPipeline& RedisPipeline::hget(std::string_view key, std::string_view field) {
    return appendCommandArgs(*this, "HGET", key, field);
}

RedisPipeline& RedisPipeline::hset(std::string_view key, std::string_view field, std::string_view value) {
    return appendCommandArgs(*this, "HSET", key, field, value);
}

RedisPipeline& RedisPipeline::hdel(std::string_view key, std::string_view field) {
    return appendCommandArgs(*this, "HDEL", key, field);
}

RedisPipeline& RedisPipeline::hexists(std::string_view key, std::string_view field) {
    return appendCommandArgs(*this, "HEXISTS", key, field);
}

RedisPipeline& RedisPipeline::hlen(std::string_view key) {
    return appendCommandArgs(*this, "HLEN", key);
}

RedisPipeline& RedisPipeline::hgetAll(std::string_view key) {
    return appendCommandArgs(*this, "HGETALL", key);
}

RedisPipeline& RedisPipeline::lpush(std::string_view key, std::string_view value) {
    return appendCommandArgs(*this, "LPUSH", key, value);
}

RedisPipeline& RedisPipeline::rpush(std::string_view key, std::string_view value) {
    return appendCommandArgs(*this, "RPUSH", key, value);
}

RedisPipeline& RedisPipeline::lpop(std::string_view key) {
    return appendCommandArgs(*this, "LPOP", key);
}

RedisPipeline& RedisPipeline::rpop(std::string_view key) {
    return appendCommandArgs(*this, "RPOP", key);
}

RedisPipeline& RedisPipeline::llen(std::string_view key) {
    return appendCommandArgs(*this, "LLEN", key);
}

RedisPipeline& RedisPipeline::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    requireActive();
    auto startValue = detail::redisIntString(start, resource());
    auto stopValue = detail::redisIntString(stop, resource());
    return appendCommandArgs(*this, "LRANGE", key, std::string_view(startValue), std::string_view(stopValue));
}

RedisPipeline& RedisPipeline::sadd(std::string_view key, std::string_view member) {
    return appendCommandArgs(*this, "SADD", key, member);
}

RedisPipeline& RedisPipeline::srem(std::string_view key, std::string_view member) {
    return appendCommandArgs(*this, "SREM", key, member);
}

RedisPipeline& RedisPipeline::smembers(std::string_view key) {
    return appendCommandArgs(*this, "SMEMBERS", key);
}

RedisPipeline& RedisPipeline::scard(std::string_view key) {
    return appendCommandArgs(*this, "SCARD", key);
}

RedisPipeline& RedisPipeline::zadd(std::string_view key, double score, std::string_view member) {
    requireActive();
    auto scoreValue = detail::redisScoreString(score, resource());
    return appendCommandArgs(*this, "ZADD", key, std::string_view(scoreValue), member);
}

RedisPipeline& RedisPipeline::zrem(std::string_view key, std::string_view member) {
    return appendCommandArgs(*this, "ZREM", key, member);
}

RedisPipeline& RedisPipeline::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    requireActive();
    auto startValue = detail::redisIntString(start, resource());
    auto stopValue = detail::redisIntString(stop, resource());
    return appendCommandArgs(*this, "ZRANGE", key, std::string_view(startValue), std::string_view(stopValue));
}

RedisPipeline& RedisPipeline::zscore(std::string_view key, std::string_view member) {
    return appendCommandArgs(*this, "ZSCORE", key, member);
}

RedisPipeline& RedisPipeline::zcard(std::string_view key) {
    return appendCommandArgs(*this, "ZCARD", key);
}

Task<std::pmr::vector<RedisValue>> RedisPipeline::executeOwned(
    detail::RedisPool& pool,
    std::pmr::vector<Command> commands,
    std::pmr::memory_resource* resource) {
    co_return co_await pool.executePipeline(
        std::span<const Command>(commands),
        resource);
}

ScopedOperation<std::pmr::vector<RedisValue>> RedisPipeline::exec() && {
    requireActive();
    auto* commandResource = resource();
    auto& pool = consumePool();
    return detail::makeScopedOperation(
        operationScope(),
        executeOwned(pool, std::move(commands_), commandResource));
}

}  // namespace ruvia
