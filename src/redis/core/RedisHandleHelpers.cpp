#include "RedisHandleHelpers.h"

#include "RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia::detail {

std::string_view redisValueString(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
    return value.string();
}

void throwIfRedisError(const RedisValue& value) {
    if (value.kind() == RedisValue::Kind::kError) {
        throw RedisError(RedisError::Code::kCommandError, value.string());
    }
}

bool redisAsciiEqualsIgnoreCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = left[i] >= 'A' && left[i] <= 'Z' ? static_cast<char>(left[i] + ('a' - 'A')) : left[i];
        const auto b = right[i] >= 'A' && right[i] <= 'Z' ? static_cast<char>(right[i] + ('a' - 'A')) : right[i];
        if (a != b) {
            return false;
        }
    }
    return true;
}

Task<RedisValue> executeOwnedRedisCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    return pool.executeOwned(std::move(args), resource);
}

Task<std::optional<std::pmr::string>> redisStringCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    if (value.null()) {
        co_return std::nullopt;
    }
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

Task<std::int64_t> redisIntegerCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    co_return value.integer();
}

Task<std::pmr::vector<std::pmr::string>> redisStringArrayCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto items = value.array();
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

Task<std::pmr::vector<bool>> redisBoolArrayCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto items = value.array();
    std::pmr::vector<bool> output(resource);
    output.reserve(items.size());
    for (const auto& item : items) {
        throwIfRedisError(item);
        output.emplace_back(item.integer() != 0);
    }
    co_return output;
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> redisOptionalStringArrayCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto items = value.array();
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

Task<void> redisOkCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    if (!redisAsciiEqualsIgnoreCase(redisValueString(value), "OK")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis status reply");
    }
    co_return;
}

Task<std::pmr::string> redisStatusCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto value = co_await executeOwnedRedisCommand(pool, std::move(args), resource);
    throwIfRedisError(value);
    const auto text = redisValueString(value);
    co_return std::pmr::string(text.data(), text.size(), resource);
}

}  // namespace ruvia::detail
