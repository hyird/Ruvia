#include "ruvia/web/detail/redis/RedisHandleHelpers.h"

#include "ruvia/web/detail/redis/RedisUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"

#include <stdexcept>
#include <utility>

namespace ruvia::detail {

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

void validateRedisPooledCommand(const RedisPool& pool, std::span<const std::string_view> args, bool allowBlocking, const RedisOperationOptions* options) {
    if (args.empty() || args.front().empty()) {
        throw std::invalid_argument("redis command requires a command name");
    }
    const auto command = args.front();
    constexpr std::string_view stateful[]{"AUTH", "SELECT", "MULTI", "EXEC", "DISCARD", "WATCH", "UNWATCH", "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE", "UNSUBSCRIBE", "PUNSUBSCRIBE", "SUNSUBSCRIBE", "MONITOR", "QUIT", "RESET"};
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
    if (httpAsciiEqualsIgnoreCase(command, "XREAD") || httpAsciiEqualsIgnoreCase(command, "XREADGROUP")) {
        for (const auto arg : args.subspan(1)) {
            if (httpAsciiEqualsIgnoreCase(arg, "STREAMS")) {
                break;
            }
            blocking = blocking || httpAsciiEqualsIgnoreCase(arg, "BLOCK");
        }
    }
    if (blocking && (!allowBlocking || pool.usage() != RedisPoolUsage::kBlocking)) {
        throw std::invalid_argument("blocking redis command requires a dedicated blocking pool");
    }
    if (blocking && options != nullptr && !options->stopToken.stoppable() && !options->timeout.has_value() && !pool.hasCommandTimeout()) {
        throw std::invalid_argument("raw blocking redis command requires a StopToken or finite command timeout");
    }
}

Task<RedisValue> executeOwnedRedisCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    return pool.executeOwned(std::move(args), resource);
}

Task<RedisValue> executeOwnedRedisCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, RedisOperationOptions options, std::pmr::memory_resource* resource) {
    return pool.executeOwned(std::move(args), resource, std::move(options));
}

Task<std::optional<std::pmr::string>> redisStringCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    if (value.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

Task<std::int64_t> redisIntegerCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    co_return redisValueInteger(value);
}

Task<std::pmr::vector<std::pmr::string>> redisStringArrayCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
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

Task<std::pmr::vector<bool>> redisBoolArrayCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto items = redisValueArray(value);
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfRedisError(item);
        output.emplace_back(redisValueInteger(item) != 0);
    }
    co_return output;
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> redisOptionalStringArrayCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
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

Task<void> redisOkCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    if (!httpAsciiEqualsIgnoreCase(redisValueString(value), "OK")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis status reply");
    }
    co_return;
}

Task<std::pmr::string> redisStatusCommand(RedisPool& pool, std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

}  // namespace ruvia::detail
