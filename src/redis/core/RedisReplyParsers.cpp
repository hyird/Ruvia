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
        result.push_back(RedisKeyValue(key, fieldValue, resource));
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
        result.push_back(
            RedisScoredValue(member, parseRedisDouble(scoreText, "invalid redis score"), resource));
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
    RedisScanResult result(resource);
    result.cursor_ = cursor;
    result.values_.reserve(values.size());
    for (const auto& item : values) {
        const auto text = redisValueString(item);
        emplaceRedisString(result.values_, text);
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
    RedisHashScanResult result(resource);
    result.cursor_ = cursor;
    result.entries_.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto key = redisValueString(values[i]);
        const auto fieldValue = redisValueString(values[i + 1]);
        result.entries_.push_back(RedisKeyValue(key, fieldValue, resource));
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
    RedisZScanResult result(resource);
    result.cursor_ = cursor;
    result.entries_.reserve(values.size() / 2);
    for (std::size_t i = 0; i < values.size(); i += 2) {
        const auto member = redisValueString(values[i]);
        const auto scoreText = redisValueString(values[i + 1]);
        result.entries_.push_back(
            RedisScoredValue(member, parseRedisDouble(scoreText, "invalid redis zscan score"), resource));
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
    return RedisKeyValue(key, item, resource);
}

}  // namespace ruvia::detail
