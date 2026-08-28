#include "ruvia/web/detail/redis/RedisHandleCommandOps.h"

#include "ruvia/web/detail/redis/RedisHandleHelpers.h"
#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::detail {

Task<void> executeRedisPing(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto reply = co_await redisStatusCommand(std::move(executor), std::move(args), resource);
    if (!httpAsciiEqualsIgnoreCase(reply, "PONG")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis ping reply");
    }
}

Task<RedisSetResult> executeRedisSet(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, RedisSetOptions options, std::pmr::memory_resource* resource) {
    auto reply = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(reply);
    const auto returnsPrevious = redisSetReturnsPrevious(options.previousValue);
    if (reply.null()) {
        const bool applied = returnsPrevious && (!options.condition.has_value() || *options.condition == RedisSetCondition::kIfAbsent);
        co_return RedisTypesAccess::setResult(applied);
    }
    const auto text = redisValueString(reply);
    if (!returnsPrevious) {
        if (!httpAsciiEqualsIgnoreCase(text, "OK")) {
            throw RedisError(RedisError::Code::kCommandError, "unexpected redis set reply");
        }
        co_return RedisTypesAccess::setResult(true);
    }
    const bool applied = !options.condition.has_value() || *options.condition != RedisSetCondition::kIfAbsent;
    co_return RedisTypesAccess::setResult(applied, std::pmr::string(text.data(), text.size(), resource));
}

Task<bool> executeRedisIntegerBool(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto reply = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return redisValueIntegerBool(reply);
}

Task<std::pmr::vector<RedisKeyValue>> executeRedisKeyValueArray(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::string_view context, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return parseRedisKeyValueArray(value, resource, context);
}

Task<std::pmr::vector<RedisScoredValue>> executeRedisScoredArray(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    co_return parseRedisScoredArray(value, resource);
}

Task<std::optional<double>> executeRedisOptionalDouble(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::string_view context, std::pmr::memory_resource* resource) {
    auto reply = co_await redisStringCommand(std::move(executor), std::move(args), resource);
    if (!reply) {
        co_return std::nullopt;
    }
    co_return parseRedisDouble(*reply, context);
}

}  // namespace ruvia::detail
