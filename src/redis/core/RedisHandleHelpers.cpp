#include "RedisHandleHelpers.h"

#include "RedisUtils.h"

#include <charconv>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

[[nodiscard]] std::uint64_t parseRedisCursor(std::string_view value) {
    std::uint64_t cursor = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), cursor);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw RedisError(RedisError::Code::kProtocolError, "invalid redis scan cursor");
    }
    return cursor;
}

}  // namespace

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

std::pmr::vector<std::pmr::string> ownRedisArgs(
    std::span<const std::string_view> args,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> owned(resource);
    owned.reserve(args.size());
    for (const auto arg : args) {
        owned.emplace_back(std::pmr::string(arg.data(), arg.size(), resource));
    }
    return owned;
}

std::pmr::vector<std::pmr::string> ownRedisArgs(
    std::initializer_list<std::string_view> args,
    std::pmr::memory_resource* resource) {
    return ownRedisArgs(std::span<const std::string_view>(args.begin(), args.size()), resource);
}

Task<RedisValue> executeOwnedRedisCommand(
    RedisPool& pool,
    std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource) {
    auto views = viewRedisArgs(args, resource);
    co_return co_await pool.execute(std::span<const std::string_view>(views.data(), views.size()), resource);
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
            output.emplace_back(std::pmr::string(text.data(), text.size(), resource));
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
            output.emplace_back(std::pmr::string(text.data(), text.size(), resource));
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

std::pmr::string redisSecondsString(std::chrono::seconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendRedisNumber(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

std::pmr::string redisMillisecondsString(std::chrono::milliseconds ttl, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendRedisNumber(output, static_cast<std::int64_t>(ttl.count()));
    return output;
}

std::pmr::string redisCursorString(std::uint64_t cursor, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendRedisNumber(output, cursor);
    return output;
}

std::pmr::vector<std::pmr::string> redisCommandWithKeys(
    std::string_view command,
    std::span<const std::string_view> keys,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 1);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisMsetArgs(
    std::span<const std::pair<std::string_view, std::string_view>> items,
    std::pmr::memory_resource* resource) {
    if (items.empty()) {
        throw std::invalid_argument("redis mset requires at least one item");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(items.size() * 2 + 1);
    args.emplace_back(std::pmr::string("MSET", 4, resource));
    for (const auto& [key, value] : items) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
        args.emplace_back(std::pmr::string(value.data(), value.size(), resource));
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisHsetFieldsArgs(
    std::string_view key,
    std::span<const std::pair<std::string_view, std::string_view>> fields,
    std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() * 2 + 2);
    args.emplace_back(std::pmr::string("HSET", 4, resource));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    for (const auto& [field, value] : fields) {
        args.emplace_back(std::pmr::string(field.data(), field.size(), resource));
        args.emplace_back(std::pmr::string(value.data(), value.size(), resource));
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisCommandWithKeyFields(
    std::string_view command,
    std::string_view key,
    std::span<const std::string_view> fields,
    std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() + 2);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    for (const auto field : fields) {
        args.emplace_back(std::pmr::string(field.data(), field.size(), resource));
    }
    return args;
}

void appendRedisScanOptions(
    std::pmr::vector<std::pmr::string>& args,
    const RedisScanOptions& options,
    std::pmr::memory_resource* resource) {
    if (!options.match.empty()) {
        args.emplace_back(std::pmr::string("MATCH", 5, resource));
        args.emplace_back(std::pmr::string(options.match.data(), options.match.size(), resource));
    }
    if (options.count != 0) {
        args.emplace_back(std::pmr::string("COUNT", 5, resource));
        std::pmr::string count(resource);
        appendRedisNumber(count, options.count);
        args.emplace_back(std::move(count));
    }
}

double parseRedisDouble(std::string_view value, std::string_view context) {
    double output = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), output);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        throw RedisError(RedisError::Code::kProtocolError, context);
    }
    return output;
}

std::pmr::vector<RedisKeyValue> parseRedisKeyValueArray(
    const RedisValue& value,
    std::pmr::memory_resource* resource,
    std::string_view context) {
    throwIfRedisError(value);
    const auto values = value.array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, context);
    }
    std::pmr::vector<RedisKeyValue> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redisValueString(values[i]);
        const auto fieldValue = redisValueString(values[i + 1]);
        result.push_back(RedisKeyValue{
            std::pmr::string(key.data(), key.size(), resource),
            std::pmr::string(fieldValue.data(), fieldValue.size(), resource)});
    }
    return result;
}

std::pmr::vector<RedisScoredValue> parseRedisScoredArray(
    const RedisValue& value,
    std::pmr::memory_resource* resource) {
    throwIfRedisError(value);
    const auto values = value.array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis scored array reply");
    }
    std::pmr::vector<RedisScoredValue> result(resource);
    result.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redisValueString(values[i]);
        const auto scoreText = redisValueString(values[i + 1]);
        result.push_back(RedisScoredValue{
            std::pmr::string(member.data(), member.size(), resource),
            parseRedisDouble(scoreText, "invalid redis score")});
    }
    return result;
}

RedisScanResult parseRedisScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfRedisError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis scan reply");
    }
    const auto cursor = parseRedisCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    RedisScanResult result{.cursor = cursor, .values = std::pmr::vector<std::pmr::string>(resource)};
    result.values.reserve(values.size());
    for (const auto& item : values) {
        const auto text = redisValueString(item);
        result.values.emplace_back(std::pmr::string(text.data(), text.size(), resource));
    }
    return result;
}

RedisHashScanResult parseRedisHashScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfRedisError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis hscan reply");
    }
    const auto cursor = parseRedisCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis hscan entry count");
    }
    RedisHashScanResult result{.cursor = cursor, .entries = std::pmr::vector<RedisKeyValue>(resource)};
    result.entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redisValueString(values[i]);
        const auto fieldValue = redisValueString(values[i + 1]);
        result.entries.push_back(RedisKeyValue{
            std::pmr::string(key.data(), key.size(), resource),
            std::pmr::string(fieldValue.data(), fieldValue.size(), resource)});
    }
    return result;
}

RedisZScanResult parseRedisZScanResult(const RedisValue& value, std::pmr::memory_resource* resource) {
    throwIfRedisError(value);
    const auto root = value.array();
    if (root.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis zscan reply");
    }
    const auto cursor = parseRedisCursor(redisValueString(root[0]));
    const auto values = root[1].array();
    if (values.size() % 2 != 0) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis zscan entry count");
    }
    RedisZScanResult result{.cursor = cursor, .entries = std::pmr::vector<RedisScoredValue>(resource)};
    result.entries.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redisValueString(values[i]);
        const auto scoreText = redisValueString(values[i + 1]);
        result.entries.push_back(RedisScoredValue{
            std::pmr::string(member.data(), member.size(), resource),
            parseRedisDouble(scoreText, "invalid redis zscan score")});
    }
    return result;
}

std::pmr::vector<std::pmr::string> redisEvalArgs(
    std::string_view command,
    std::string_view script,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> argv,
    std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(3 + keys.size() + argv.size());
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    args.emplace_back(std::pmr::string(script.data(), script.size(), resource));
    args.emplace_back(redisIntString(static_cast<std::int64_t>(keys.size()), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    for (const auto arg : argv) {
        args.emplace_back(std::pmr::string(arg.data(), arg.size(), resource));
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisBlockingPopArgs(
    std::string_view command,
    std::span<const std::string_view> keys,
    std::chrono::seconds timeout,
    std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 2);
    args.emplace_back(std::pmr::string(command.data(), command.size(), resource));
    for (const auto key : keys) {
        args.emplace_back(std::pmr::string(key.data(), key.size(), resource));
    }
    args.emplace_back(redisSecondsString(timeout, resource));
    return args;
}

std::optional<RedisKeyValue> parseRedisBlockingPopReply(
    const RedisValue& value,
    std::pmr::memory_resource* resource) {
    throwIfRedisError(value);
    if (value.null()) {
        return std::nullopt;
    }
    const auto items = value.array();
    if (items.size() != 2) {
        throw RedisError(RedisError::Code::kProtocolError, "unexpected redis blocking pop reply");
    }
    const auto key = redisValueString(items[0]);
    const auto item = redisValueString(items[1]);
    return RedisKeyValue{
        std::pmr::string(key.data(), key.size(), resource),
        std::pmr::string(item.data(), item.size(), resource)};
}

}  // namespace ruvia::detail
