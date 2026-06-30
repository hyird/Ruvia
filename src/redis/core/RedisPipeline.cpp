#include "ruvia/redis/Redis.h"

#include "../RedisInternal.h"
#include "RedisUtils.h"

#include <stdexcept>

namespace ruvia {

RedisPipeline::RedisPipeline(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource) noexcept
    : pool_(&pool),
      resource_(detail::pmrResourceOrDefault(resource)),
      commands_(resource_) {}

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

RedisPipeline& RedisPipeline::command(std::initializer_list<std::string_view> args) {
    return command(std::span<const std::string_view>(args.begin(), args.size()));
}

RedisPipeline& RedisPipeline::command(std::span<const std::string_view> args) {
    appendCommand(commands_, resource_, args);
    return *this;
}

RedisPipeline& RedisPipeline::get(std::string_view key) {
    return command({"GET", key});
}

RedisPipeline& RedisPipeline::set(std::string_view key, std::string_view value) {
    return command({"SET", key, value});
}

RedisPipeline& RedisPipeline::getDel(std::string_view key) {
    return command({"GETDEL", key});
}

RedisPipeline& RedisPipeline::getSet(std::string_view key, std::string_view value) {
    return command({"GETSET", key, value});
}

RedisPipeline& RedisPipeline::append(std::string_view key, std::string_view value) {
    return command({"APPEND", key, value});
}

RedisPipeline& RedisPipeline::strlen(std::string_view key) {
    return command({"STRLEN", key});
}

RedisPipeline& RedisPipeline::del(std::string_view key) {
    return command({"DEL", key});
}

RedisPipeline& RedisPipeline::unlink(std::string_view key) {
    return command({"UNLINK", key});
}

RedisPipeline& RedisPipeline::exists(std::string_view key) {
    return command({"EXISTS", key});
}

RedisPipeline& RedisPipeline::touch(std::string_view key) {
    return command({"TOUCH", key});
}

RedisPipeline& RedisPipeline::type(std::string_view key) {
    return command({"TYPE", key});
}

RedisPipeline& RedisPipeline::rename(std::string_view key, std::string_view newKey) {
    return command({"RENAME", key, newKey});
}

RedisPipeline& RedisPipeline::renameNx(std::string_view key, std::string_view newKey) {
    return command({"RENAMENX", key, newKey});
}

RedisPipeline& RedisPipeline::incr(std::string_view key) {
    return command({"INCR", key});
}

RedisPipeline& RedisPipeline::incrBy(std::string_view key, std::int64_t value) {
    auto amount = detail::redisIntString(value, resource_);
    return command({"INCRBY", key, std::string_view(amount)});
}

RedisPipeline& RedisPipeline::decr(std::string_view key) {
    return command({"DECR", key});
}

RedisPipeline& RedisPipeline::decrBy(std::string_view key, std::int64_t value) {
    auto amount = detail::redisIntString(value, resource_);
    return command({"DECRBY", key, std::string_view(amount)});
}

RedisPipeline& RedisPipeline::hget(std::string_view key, std::string_view field) {
    return command({"HGET", key, field});
}

RedisPipeline& RedisPipeline::hset(std::string_view key, std::string_view field, std::string_view value) {
    return command({"HSET", key, field, value});
}

RedisPipeline& RedisPipeline::hdel(std::string_view key, std::string_view field) {
    return command({"HDEL", key, field});
}

RedisPipeline& RedisPipeline::hexists(std::string_view key, std::string_view field) {
    return command({"HEXISTS", key, field});
}

RedisPipeline& RedisPipeline::hlen(std::string_view key) {
    return command({"HLEN", key});
}

RedisPipeline& RedisPipeline::hgetAll(std::string_view key) {
    return command({"HGETALL", key});
}

RedisPipeline& RedisPipeline::lpush(std::string_view key, std::string_view value) {
    return command({"LPUSH", key, value});
}

RedisPipeline& RedisPipeline::rpush(std::string_view key, std::string_view value) {
    return command({"RPUSH", key, value});
}

RedisPipeline& RedisPipeline::lpop(std::string_view key) {
    return command({"LPOP", key});
}

RedisPipeline& RedisPipeline::rpop(std::string_view key) {
    return command({"RPOP", key});
}

RedisPipeline& RedisPipeline::llen(std::string_view key) {
    return command({"LLEN", key});
}

RedisPipeline& RedisPipeline::lrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return command({"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)});
}

RedisPipeline& RedisPipeline::sadd(std::string_view key, std::string_view member) {
    return command({"SADD", key, member});
}

RedisPipeline& RedisPipeline::srem(std::string_view key, std::string_view member) {
    return command({"SREM", key, member});
}

RedisPipeline& RedisPipeline::smembers(std::string_view key) {
    return command({"SMEMBERS", key});
}

RedisPipeline& RedisPipeline::scard(std::string_view key) {
    return command({"SCARD", key});
}

RedisPipeline& RedisPipeline::zadd(std::string_view key, double score, std::string_view member) {
    auto scoreValue = detail::redisScoreString(score, resource_);
    return command({"ZADD", key, std::string_view(scoreValue), member});
}

RedisPipeline& RedisPipeline::zrem(std::string_view key, std::string_view member) {
    return command({"ZREM", key, member});
}

RedisPipeline& RedisPipeline::zrange(std::string_view key, std::int64_t start, std::int64_t stop) {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return command({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)});
}

RedisPipeline& RedisPipeline::zscore(std::string_view key, std::string_view member) {
    return command({"ZSCORE", key, member});
}

RedisPipeline& RedisPipeline::zcard(std::string_view key) {
    return command({"ZCARD", key});
}

Task<std::pmr::vector<RedisValue>> RedisPipeline::exec() {
    if (pool_ == nullptr) {
        throw std::logic_error("redis pipeline is empty");
    }
    co_return co_await pool_->executePipeline(std::span<const Command>(commands_.data(), commands_.size()), resource_);
}

}  // namespace ruvia
