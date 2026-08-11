#include "ruvia/web/detail/redis/RedisHandleHelpers.h"

#include "ruvia/web/detail/redis/RedisUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"

#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isRedisToken(std::string_view value, std::string_view token) {
    return httpAsciiEqualsIgnoreCase(value, token);
}

[[nodiscard]] bool xreadOptionsContainBlocking(std::span<const std::string_view> args, std::size_t firstOption, bool allowNoAck) {
    for (std::size_t i = firstOption; i < args.size();) {
        const auto token = args[i];
        if (isRedisToken(token, "STREAMS")) {
            return false;
        }
        if (isRedisToken(token, "BLOCK")) {
            return true;
        }
        if (isRedisToken(token, "COUNT")) {
            i += 2;
            continue;
        }
        if (allowNoAck && isRedisToken(token, "NOACK")) {
            ++i;
            continue;
        }
        return false;
    }
    return false;
}

[[nodiscard]] bool xreadCommandIsBlocking(std::span<const std::string_view> args) {
    return xreadOptionsContainBlocking(args, 1, false);
}

[[nodiscard]] bool xreadGroupCommandIsBlocking(std::span<const std::string_view> args) {
    if (args.size() < 4 || !isRedisToken(args[1], "GROUP")) {
        return false;
    }
    return xreadOptionsContainBlocking(args, 4, true);
}

}  // namespace

std::string_view redisValueString(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
    if (value.kind() != RedisValue::Kind::kString) {
        throw RedisError(RedisError::Code::kProtocolError, "redis reply is not a string");
    }
    return value.string();
}

std::int64_t redisValueInteger(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
    if (value.kind() != RedisValue::Kind::kInteger) {
        throw RedisError(RedisError::Code::kProtocolError, "redis reply is not an integer");
    }
    return value.integer();
}

bool redisValueIntegerBool(const RedisValue& value) {
    const auto integer = redisValueInteger(value);
    if (integer == 0) {
        return false;
    }
    if (integer == 1) {
        return true;
    }
    throw RedisError(RedisError::Code::kProtocolError, "redis boolean reply is not 0 or 1");
}

std::span<const RedisValue> redisValueArray(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
    if (value.kind() != RedisValue::Kind::kArray) {
        throw RedisError(RedisError::Code::kProtocolError, "redis reply is not an array");
    }
    return value.array();
}

void throwIfRedisError(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
}

void validateRedisOperationOptions(const RedisOperationOptions& options) {
    if (options.timeout.has_value() && options.timeout->count() <= 0) {
        throw std::invalid_argument("redis operation timeout must be greater than zero");
    }
}

RedisOperationOptions mergeRedisOperationOptions(const RedisOperationOptions& base, RedisOperationOptions overrides) {
    RedisOperationOptions merged = base;
    if (overrides.timeout.has_value() && (!merged.timeout.has_value() || *overrides.timeout < *merged.timeout)) {
        merged.timeout = overrides.timeout;
    }
    merged.stopToken = combineStopTokens(base.stopToken, std::move(overrides.stopToken));
    return merged;
}

bool validateRedisPooledCommand(std::span<const std::string_view> args, bool allowBlocking) {
    if (args.empty() || args.front().empty()) {
        throw std::invalid_argument("redis command requires a command name");
    }
    const auto command = args.front();
    constexpr std::string_view stateful[]{
        "ASKING",
        "AUTH",
        "CLIENT",
        "DISCARD",
        "EXEC",
        "HELLO",
        "MONITOR",
        "MULTI",
        "PSUBSCRIBE",
        "PUNSUBSCRIBE",
        "QUIT",
        "READONLY",
        "READWRITE",
        "RESET",
        "SELECT",
        "SSUBSCRIBE",
        "SUBSCRIBE",
        "SUNSUBSCRIBE",
        "UNWATCH",
        "WATCH",
    };
    for (const auto name : stateful) {
        if (httpAsciiEqualsIgnoreCase(command, name)) {
            throw std::invalid_argument("stateful redis commands are not allowed through pooled command()");
        }
    }

    bool blocking = false;
    constexpr std::string_view alwaysBlocking[]{"BLPOP", "BRPOP", "BRPOPLPUSH", "BLMOVE", "BLMPOP", "BZPOPMIN", "BZPOPMAX", "BZMPOP", "WAIT", "WAITAOF"};
    for (const auto name : alwaysBlocking) {
        blocking = blocking || httpAsciiEqualsIgnoreCase(command, name);
    }
    if (httpAsciiEqualsIgnoreCase(command, "XREAD")) {
        blocking = blocking || xreadCommandIsBlocking(args);
    }
    if (httpAsciiEqualsIgnoreCase(command, "XREADGROUP")) {
        blocking = blocking || xreadGroupCommandIsBlocking(args);
    }
    if (blocking && !allowBlocking) {
        throw std::invalid_argument("blocking redis command is not allowed in a pipeline or transaction");
    }
    return blocking;
}

Task<RedisValue> executeOwnedRedisCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    return executor.pool->executeOwned(std::move(args), resource, std::move(executor.options));
}

Task<RedisValue> executeOwnedRedisCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    return pool.executeOwned(std::move(args), resource, std::move(options));
}

Task<std::optional<std::pmr::string>> redisStringCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    if (value.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

Task<std::int64_t> redisIntegerCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    co_return redisValueInteger(value);
}

Task<std::pmr::vector<std::pmr::string>> redisStringArrayCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    const auto items = redisValueArray(value);
    std::pmr::vector<std::pmr::string> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfRedisError(item);
        if (!item.null()) {
            const auto text = redisValueString(item);
            emplaceRedisString(output, text);
        }
    }
    co_return output;
}

Task<std::pmr::vector<bool>> redisBoolArrayCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    const auto items = redisValueArray(value);
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfRedisError(item);
        output.emplace_back(redisValueIntegerBool(item));
    }
    co_return output;
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> redisOptionalStringArrayCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    const auto items = redisValueArray(value);
    std::pmr::vector<std::optional<std::pmr::string>> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfRedisError(item);
        if (item.null()) {
            output.emplace_back(std::nullopt);
        } else {
            const auto text = redisValueString(item);
            auto& stored = output.emplace_back(std::in_place, resource);
            stored->assign(text.data(), text.size());
        }
    }
    co_return output;
}

Task<void> redisOkCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    if (!httpAsciiEqualsIgnoreCase(redisValueString(value), "OK")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis status reply");
    }
    co_return;
}

Task<std::pmr::string> redisStatusCommand(RedisCommandExecutor executor, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(std::move(executor), std::move(args), resource);
    throwIfRedisError(value);
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

}  // namespace ruvia::detail
