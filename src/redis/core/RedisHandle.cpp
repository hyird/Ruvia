#include "ruvia/redis/Redis.h"

#include "RedisHandleHelpers.h"
#include "../RedisInternal.h"
#include "RedisUtils.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

RedisHandle::RedisHandle(
    detail::RedisPool& pool,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : pool_(pool),
      resource_(detail::resolveRedisResource(resource)),
      requestMemory_(requestMemory) {}

Task<RedisValue> RedisHandle::command(std::initializer_list<std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::ownRedisArgs(args, resource_), resource_);
}

Task<RedisValue> RedisHandle::command(std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::ownRedisArgs(args, resource_), resource_);
}

Task<void> RedisHandle::ping() const {
    auto reply = co_await detail::redisStatusCommand(pool_, detail::ownRedisArgs({"PING"}, resource_), resource_);
    if (!detail::redisAsciiEqualsIgnoreCase(reply, "PONG")) {
        throw RedisError(RedisError::Code::kCommandError, "unexpected redis ping reply");
    }
}

Task<std::pmr::string> RedisHandle::ping(std::string_view message) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"PING", message}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::get(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GET", key}, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::mget(std::span<const std::string_view> keys) const {
    return detail::redisOptionalStringArrayCommand(pool_, detail::redisCommandWithKeys("MGET", keys, resource_), resource_);
}

Task<void> RedisHandle::set(std::string_view key, std::string_view value) const {
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"SET", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::set(std::string_view key, std::string_view value, RedisSetOptions options) const {
    if (options.nx && options.xx) {
        throw std::invalid_argument("redis set options cannot combine NX and XX");
    }
    if (options.ttl.count() > 0 && options.keepTtl) {
        throw std::invalid_argument("redis set options cannot combine TTL and KEEPTTL");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(8);
    args.emplace_back(std::pmr::string("SET", 3, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::pmr::string(value.data(), value.size(), resource_));
    std::pmr::string ttlValue(resource_);
    if (options.ttl.count() > 0) {
        ttlValue = detail::redisMillisecondsString(options.ttl, resource_);
        args.emplace_back(std::pmr::string("PX", 2, resource_));
        args.emplace_back(std::pmr::string(ttlValue.data(), ttlValue.size(), resource_));
    }
    if (options.nx) {
        args.emplace_back(std::pmr::string("NX", 2, resource_));
    }
    if (options.xx) {
        args.emplace_back(std::pmr::string("XX", 2, resource_));
    }
    if (options.get) {
        args.emplace_back(std::pmr::string("GET", 3, resource_));
    }
    if (options.keepTtl) {
        args.emplace_back(std::pmr::string("KEEPTTL", 7, resource_));
    }

    auto reply = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    detail::throwIfRedisError(reply);
    if (reply.null()) {
        co_return std::nullopt;
    }
    const auto text = detail::redisValueString(reply);
    if (!options.get) {
        if (!detail::redisAsciiEqualsIgnoreCase(text, "OK")) {
            throw RedisError(RedisError::Code::kCommandError, "unexpected redis set reply");
        }
        co_return std::nullopt;
    }
    co_return std::pmr::string(text.data(), text.size(), resource_);
}

Task<void> RedisHandle::mset(std::span<const std::pair<std::string_view, std::string_view>> items) const {
    return detail::redisOkCommand(pool_, detail::redisMsetArgs(items, resource_), resource_);
}

Task<void> RedisHandle::setEx(std::string_view key, std::chrono::seconds ttl, std::string_view value) const {
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"SETEX", key, std::string_view(ttlValue), value}, resource_), resource_);
}

Task<bool> RedisHandle::setNx(std::string_view key, std::string_view value) const {
    auto args = detail::ownRedisArgs({"SET", key, value, "NX"}, resource_);
    auto reply = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    detail::throwIfRedisError(reply);
    if (reply.null()) {
        co_return false;
    }
    co_return detail::redisAsciiEqualsIgnoreCase(detail::redisValueString(reply), "OK");
}

Task<std::optional<std::pmr::string>> RedisHandle::getDel(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GETDEL", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::getSet(std::string_view key, std::string_view value) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"GETSET", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::append(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"APPEND", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::strlen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"STRLEN", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incrBy(std::string_view key, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"INCRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decr(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DECR", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::decrBy(std::string_view key, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DECRBY", key, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::del(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"DEL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::unlink(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"UNLINK", key}, resource_), resource_);
}

Task<bool> RedisHandle::exists(std::string_view key) const {
    auto args = detail::ownRedisArgs({"EXISTS", key}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::touch(std::string_view key) const {
    auto args = detail::ownRedisArgs({"TOUCH", key}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::pmr::string> RedisHandle::type(std::string_view key) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"TYPE", key}, resource_), resource_);
}

Task<void> RedisHandle::rename(std::string_view key, std::string_view newKey) const {
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"RENAME", key, newKey}, resource_), resource_);
}

Task<bool> RedisHandle::renameNx(std::string_view key, std::string_view newKey) const {
    auto args = detail::ownRedisArgs({"RENAMENX", key, newKey}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::expire(std::string_view key, std::chrono::seconds ttl) const {
    auto ttlValue = detail::redisSecondsString(ttl, resource_);
    auto args = detail::ownRedisArgs({"EXPIRE", key, std::string_view(ttlValue)}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::expireAt(std::string_view key, std::chrono::seconds unixTime) const {
    auto value = detail::redisSecondsString(unixTime, resource_);
    auto args = detail::ownRedisArgs({"EXPIREAT", key, std::string_view(value)}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<bool> RedisHandle::persist(std::string_view key) const {
    auto args = detail::ownRedisArgs({"PERSIST", key}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::int64_t> RedisHandle::ttl(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"TTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::pttl(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"PTTL", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::incr(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"INCR", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::hget(std::string_view key, std::string_view field) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"HGET", key, field}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::string_view field, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HSET", key, field, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hset(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields) const {
    return detail::redisIntegerCommand(pool_, detail::redisHsetFieldsArgs(key, fields, resource_), resource_);
}

Task<std::pmr::vector<std::optional<std::pmr::string>>> RedisHandle::hmget(std::string_view key, std::span<const std::string_view> fields) const {
    return detail::redisOptionalStringArrayCommand(pool_, detail::redisCommandWithKeyFields("HMGET", key, fields, resource_), resource_);
}

Task<std::pmr::vector<RedisKeyValue>> RedisHandle::hgetAll(std::string_view key) const {
    auto value = co_await detail::executeOwnedRedisCommand(pool_, detail::ownRedisArgs({"HGETALL", key}, resource_), resource_);
    co_return detail::parseRedisKeyValueArray(value, resource_, "unexpected redis hgetall reply");
}

Task<std::int64_t> RedisHandle::hdel(std::string_view key, std::string_view field) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HDEL", key, field}, resource_), resource_);
}

Task<bool> RedisHandle::hexists(std::string_view key, std::string_view field) const {
    auto args = detail::ownRedisArgs({"HEXISTS", key, field}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::int64_t> RedisHandle::hlen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hkeys(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"HKEYS", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::hvals(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"HVALS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::hincrBy(std::string_view key, std::string_view field, std::int64_t value) const {
    auto amount = detail::redisIntString(value, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"HINCRBY", key, field, std::string_view(amount)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lpush(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LPUSH", key, value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::rpush(std::string_view key, std::string_view value) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"RPUSH", key, value}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lpop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"LPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::rpop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"RPOP", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::llen(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LLEN", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::lrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"LRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::lindex(std::string_view key, std::int64_t index) const {
    auto indexValue = detail::redisIntString(index, resource_);
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"LINDEX", key, std::string_view(indexValue)}, resource_), resource_);
}

Task<void> RedisHandle::lset(std::string_view key, std::int64_t index, std::string_view value) const {
    auto indexValue = detail::redisIntString(index, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"LSET", key, std::string_view(indexValue), value}, resource_), resource_);
}

Task<void> RedisHandle::ltrim(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisOkCommand(pool_, detail::ownRedisArgs({"LTRIM", key, std::string_view(startValue), std::string_view(stopValue)}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::lrem(std::string_view key, std::int64_t count, std::string_view value) const {
    auto countValue = detail::redisIntString(count, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"LREM", key, std::string_view(countValue), value}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::sadd(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SADD", key, member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::srem(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::smembers(std::string_view key) const {
    return detail::redisStringArrayCommand(pool_, detail::ownRedisArgs({"SMEMBERS", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::scard(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"SCARD", key}, resource_), resource_);
}

Task<bool> RedisHandle::sismember(std::string_view key, std::string_view member) const {
    auto args = detail::ownRedisArgs({"SISMEMBER", key, member}, resource_);
    co_return (co_await detail::redisIntegerCommand(pool_, std::move(args), resource_)) != 0;
}

Task<std::optional<std::pmr::string>> RedisHandle::spop(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"SPOP", key}, resource_), resource_);
}

Task<std::optional<std::pmr::string>> RedisHandle::srandMember(std::string_view key) const {
    return detail::redisStringCommand(pool_, detail::ownRedisArgs({"SRANDMEMBER", key}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sinter(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SINTER", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sunion(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SUNION", keys, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::sdiff(std::span<const std::string_view> keys) const {
    return detail::redisStringArrayCommand(pool_, detail::redisCommandWithKeys("SDIFF", keys, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zadd(std::string_view key, double score, std::string_view member) const {
    auto scoreValue = detail::redisScoreString(score, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZADD", key, std::string_view(scoreValue), member}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zrem(std::string_view key, std::string_view member) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZREM", key, member}, resource_), resource_);
}

Task<std::pmr::vector<std::pmr::string>> RedisHandle::zrange(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    return detail::redisStringArrayCommand(
        pool_,
        detail::ownRedisArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue)}, resource_),
        resource_);
}

Task<std::pmr::vector<RedisScoredValue>> RedisHandle::zrangeWithScores(std::string_view key, std::int64_t start, std::int64_t stop) const {
    auto startValue = detail::redisIntString(start, resource_);
    auto stopValue = detail::redisIntString(stop, resource_);
    auto value = co_await detail::executeOwnedRedisCommand(
        pool_,
        detail::ownRedisArgs({"ZRANGE", key, std::string_view(startValue), std::string_view(stopValue), "WITHSCORES"}, resource_),
        resource_);
    co_return detail::parseRedisScoredArray(value, resource_);
}

Task<std::optional<double>> RedisHandle::zscore(std::string_view key, std::string_view member) const {
    auto reply = co_await detail::redisStringCommand(pool_, detail::ownRedisArgs({"ZSCORE", key, member}, resource_), resource_);
    if (!reply) {
        co_return std::nullopt;
    }
    co_return detail::parseRedisDouble(*reply, "invalid redis zscore reply");
}

Task<std::int64_t> RedisHandle::zcard(std::string_view key) const {
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZCARD", key}, resource_), resource_);
}

Task<std::int64_t> RedisHandle::zcount(std::string_view key, double min, double max) const {
    auto minValue = detail::redisScoreString(min, resource_);
    auto maxValue = detail::redisScoreString(max, resource_);
    return detail::redisIntegerCommand(pool_, detail::ownRedisArgs({"ZCOUNT", key, std::string_view(minValue), std::string_view(maxValue)}, resource_), resource_);
}

Task<RedisScanResult> RedisHandle::scan(RedisScanOptions options) const {
    auto cursor = detail::redisCursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(6);
    args.emplace_back(std::pmr::string("SCAN", 4, resource_));
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource_);
    auto value = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    co_return detail::parseRedisScanResult(value, resource_);
}

Task<RedisHashScanResult> RedisHandle::hscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = detail::redisCursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("HSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource_);
    auto value = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    co_return detail::parseRedisHashScanResult(value, resource_);
}

Task<RedisScanResult> RedisHandle::sscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = detail::redisCursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("SSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource_);
    auto value = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    co_return detail::parseRedisScanResult(value, resource_);
}

Task<RedisZScanResult> RedisHandle::zscan(std::string_view key, RedisScanOptions options) const {
    auto cursor = detail::redisCursorString(options.cursor, resource_);
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(7);
    args.emplace_back(std::pmr::string("ZSCAN", 5, resource_));
    args.emplace_back(std::pmr::string(key.data(), key.size(), resource_));
    args.emplace_back(std::move(cursor));
    detail::appendRedisScanOptions(args, options, resource_);
    auto value = co_await detail::executeOwnedRedisCommand(pool_, std::move(args), resource_);
    co_return detail::parseRedisZScanResult(value, resource_);
}

Task<RedisValue> RedisHandle::eval(
    std::string_view script,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::redisEvalArgs("EVAL", script, keys, args, resource_), resource_);
}

Task<RedisValue> RedisHandle::evalSha(
    std::string_view sha1,
    std::span<const std::string_view> keys,
    std::span<const std::string_view> args) const {
    return detail::executeOwnedRedisCommand(pool_, detail::redisEvalArgs("EVALSHA", sha1, keys, args, resource_), resource_);
}

Task<std::pmr::string> RedisHandle::scriptLoad(std::string_view script) const {
    return detail::redisStatusCommand(pool_, detail::ownRedisArgs({"SCRIPT", "LOAD", script}, resource_), resource_);
}

Task<std::pmr::vector<bool>> RedisHandle::scriptExists(std::span<const std::string_view> sha1s) const {
    if (sha1s.empty()) {
        throw std::invalid_argument("redis script exists requires at least one sha1");
    }
    std::pmr::vector<std::pmr::string> args(resource_);
    args.reserve(sha1s.size() + 2);
    args.emplace_back(std::pmr::string("SCRIPT", 6, resource_));
    args.emplace_back(std::pmr::string("EXISTS", 6, resource_));
    for (const auto sha1 : sha1s) {
        args.emplace_back(std::pmr::string(sha1.data(), sha1.size(), resource_));
    }
    return detail::redisBoolArrayCommand(pool_, std::move(args), resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::blpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const {
    auto args = detail::redisBlockingPopArgs("BLPOP", keys, timeout, resource_);
    auto views = detail::viewRedisArgs(args, resource_);
    const auto clientTimeout = timeout <= std::chrono::seconds(0)
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<std::chrono::milliseconds>(timeout) + std::chrono::seconds(1);
    auto reply = co_await pool_.executeWithTimeout(std::span<const std::string_view>(views.data(), views.size()), clientTimeout, resource_);
    co_return detail::parseRedisBlockingPopReply(reply, resource_);
}

Task<std::optional<RedisKeyValue>> RedisHandle::brpop(std::span<const std::string_view> keys, std::chrono::seconds timeout) const {
    auto args = detail::redisBlockingPopArgs("BRPOP", keys, timeout, resource_);
    auto views = detail::viewRedisArgs(args, resource_);
    const auto clientTimeout = timeout <= std::chrono::seconds(0)
        ? std::chrono::milliseconds(0)
        : std::chrono::duration_cast<std::chrono::milliseconds>(timeout) + std::chrono::seconds(1);
    auto reply = co_await pool_.executeWithTimeout(std::span<const std::string_view>(views.data(), views.size()), clientTimeout, resource_);
    co_return detail::parseRedisBlockingPopReply(reply, resource_);
}

RedisPipeline RedisHandle::pipeline() const {
    return RedisPipeline(pool_, resource_, requestMemory_);
}

RedisTransaction RedisHandle::transaction() const {
    return RedisTransaction(pipeline());
}

}  // namespace ruvia
