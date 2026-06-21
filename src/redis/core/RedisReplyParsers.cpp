#include "RedisHandleHelpers.h"

#include "RedisUtils.h"

#include <charconv>

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
        emplaceRedisString(result.values, text);
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
