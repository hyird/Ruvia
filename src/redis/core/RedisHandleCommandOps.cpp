#include "RedisHandleCommandOps.h"

#include "RedisHandleHelpers.h"

namespace ruvia::detail {

Task<void> executeRedisPing(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto reply = co_await redisStatusCommand(pool, std::move(args), resource);
    if (!redisAsciiEqualsIgnoreCase(reply, "PONG")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis ping reply");
    }
}

Task<std::optional<std::pmr::string>> executeRedisSetWithOptions(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    bool get,
    std::pmr::memory_resource* resource) {
    auto reply = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(reply);
    if (reply.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(reply);
    if (!get) {
        if (!redisAsciiEqualsIgnoreCase(text, "OK")) {
            throw RedisError(RedisError::Code::kCommandError, "unexpected redis set reply");
        }
        co_return std::nullopt;
    }
    co_return std::pmr::string(text.data(), text.size(), resource);
}

Task<bool> executeRedisSetNx(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto reply = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(reply);
    if (reply.null()) {
        co_return false;
    }
    co_return redisAsciiEqualsIgnoreCase(redisValueString(reply), "OK");
}

Task<bool> executeRedisIntegerBool(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    co_return (co_await redisIntegerCommand(pool, std::move(args), resource)) != 0;
}

Task<std::pmr::vector<RedisKeyValue>> executeRedisKeyValueArray(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::string_view context,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    co_return parseRedisKeyValueArray(value, resource, context);
}

Task<std::pmr::vector<RedisScoredValue>> executeRedisScoredArray(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    co_return parseRedisScoredArray(value, resource);
}

Task<std::optional<double>> executeRedisOptionalDouble(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::string_view context,
    std::pmr::memory_resource* resource) {
    auto reply = co_await redisStringCommand(pool, std::move(args), resource);
    if (!reply) {
        co_return std::nullopt;
    }
    co_return parseRedisDouble(*reply, context);
}

}  // namespace ruvia::detail
