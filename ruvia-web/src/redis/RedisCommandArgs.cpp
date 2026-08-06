#include "ruvia/web/detail/redis/RedisHandleHelpers.h"

#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <stdexcept>

namespace ruvia::detail {

std::pmr::vector<std::pmr::string> ownRedisArgs(std::span<const std::string_view> args, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> owned(resource);
    owned.reserve(args.size());
    for (const auto arg : args) {
        emplaceRedisString(owned, arg);
    }
    return owned;
}

std::pmr::vector<std::pmr::string> ownRedisArgs(std::initializer_list<std::string_view> args, std::pmr::memory_resource* resource) {
    return ownRedisArgs(std::span<const std::string_view>(args.begin(), args.size()), resource);
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

std::pmr::string redisCursorString(std::optional<RedisScanCursor> cursor, std::pmr::memory_resource* resource) {
    std::pmr::string output(resource);
    appendRedisNumber(output, cursor.has_value() ? RedisTypesAccess::cursorValue(*cursor) : 0);
    return output;
}

std::pmr::vector<std::pmr::string> redisCommandWithKeys(std::string_view command, std::span<const std::string_view> keys, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 1);
    emplaceRedisString(args, command);
    for (const auto key : keys) {
        emplaceRedisString(args, key);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisMsetArgs(std::span<const std::pair<std::string_view, std::string_view>> items, std::pmr::memory_resource* resource) {
    if (items.empty()) {
        throw std::invalid_argument("redis mset requires at least one item");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(items.size() * 2 + 1);
    emplaceRedisString(args, "MSET");
    for (const auto& [key, value] : items) {
        emplaceRedisString(args, key);
        emplaceRedisString(args, value);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisSetArgs(std::string_view key, std::string_view value, const RedisSetOptions& options, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(8);
    emplaceRedisString(args, "SET");
    emplaceRedisString(args, key);
    emplaceRedisString(args, value);
    if (options.expiration) {
        if (const auto* duration = options.expiration->duration()) {
            emplaceRedisString(args, "PX");
            args.emplace_back(redisMillisecondsString(*duration, resource));
        }
    }
    if (options.condition) {
        switch (*options.condition) {
            case RedisSetCondition::kIfAbsent:
                emplaceRedisString(args, "NX");
                break;
            case RedisSetCondition::kIfPresent:
                emplaceRedisString(args, "XX");
                break;
        }
    }
    if (options.returnPrevious) {
        emplaceRedisString(args, "GET");
    }
    if (options.expiration && options.expiration->keepsExisting()) {
        emplaceRedisString(args, "KEEPTTL");
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisHsetFieldsArgs(std::string_view key, std::span<const std::pair<std::string_view, std::string_view>> fields, std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis hset requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() * 2 + 2);
    emplaceRedisString(args, "HSET");
    emplaceRedisString(args, key);
    for (const auto& [field, value] : fields) {
        emplaceRedisString(args, field);
        emplaceRedisString(args, value);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisCommandWithKeyFields(std::string_view command, std::string_view key, std::span<const std::string_view> fields, std::pmr::memory_resource* resource) {
    if (fields.empty()) {
        throw std::invalid_argument("redis command requires at least one field");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(fields.size() + 2);
    emplaceRedisString(args, command);
    emplaceRedisString(args, key);
    for (const auto field : fields) {
        emplaceRedisString(args, field);
    }
    return args;
}

void appendRedisScanOptions(std::pmr::vector<std::pmr::string>& args, const RedisScanOptions& options, std::pmr::memory_resource* resource) {
    if (options.count.has_value() && *options.count == 0) {
        throw std::invalid_argument("configured redis scan count must be greater than zero");
    }
    if (!options.match.empty()) {
        emplaceRedisString(args, "MATCH");
        emplaceRedisString(args, options.match);
    }
    if (options.count.has_value()) {
        emplaceRedisString(args, "COUNT");
        std::pmr::string count(resource);
        appendRedisNumber(count, *options.count);
        args.emplace_back(std::move(count));
    }
}

std::pmr::vector<std::pmr::string> redisEvalArgs(std::string_view command, std::string_view script, std::span<const std::string_view> keys, std::span<const std::string_view> argv, std::pmr::memory_resource* resource) {
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(3 + keys.size() + argv.size());
    emplaceRedisString(args, command);
    emplaceRedisString(args, script);
    args.emplace_back(redisIntString(static_cast<std::int64_t>(keys.size()), resource));
    for (const auto key : keys) {
        emplaceRedisString(args, key);
    }
    for (const auto arg : argv) {
        emplaceRedisString(args, arg);
    }
    return args;
}

std::pmr::vector<std::pmr::string> redisBlockingPopArgs(std::string_view command, std::span<const std::string_view> keys, std::chrono::seconds timeout, std::pmr::memory_resource* resource) {
    if (keys.empty()) {
        throw std::invalid_argument("redis blocking pop requires at least one key");
    }
    if (timeout.count() < 0) {
        throw std::invalid_argument("redis blocking pop timeout must not be negative");
    }
    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(keys.size() + 2);
    emplaceRedisString(args, command);
    for (const auto key : keys) {
        emplaceRedisString(args, key);
    }
    args.emplace_back(redisSecondsString(timeout, resource));
    return args;
}

std::optional<std::chrono::milliseconds> redisBlockingPopClientTimeout(std::chrono::seconds timeout) noexcept {
    if (timeout <= std::chrono::seconds::zero()) {
        return std::nullopt;
    }

    using Milliseconds = std::chrono::milliseconds;
    constexpr auto kMillisecondsPerSecond = Milliseconds(std::chrono::seconds(1)).count();
    constexpr auto kMaximumMilliseconds = Milliseconds::max().count();
    const auto seconds = timeout.count();
    if (seconds > kMaximumMilliseconds / kMillisecondsPerSecond) {
        return Milliseconds::max();
    }

    const auto milliseconds = static_cast<Milliseconds::rep>(seconds) * kMillisecondsPerSecond;
    if (milliseconds > kMaximumMilliseconds - kMillisecondsPerSecond) {
        return Milliseconds::max();
    }
    return Milliseconds(milliseconds + kMillisecondsPerSecond);
}

std::pmr::vector<std::pmr::string> redisXReadGroupArgs(std::string_view group, std::string_view consumer, std::span<const RedisStreamReadView> streams, const RedisXReadGroupOptions& options, std::pmr::memory_resource* resource) {
    if (group.empty() || consumer.empty()) {
        throw std::invalid_argument("redis xreadgroup requires a group and consumer");
    }
    if (streams.empty()) {
        throw std::invalid_argument("redis xreadgroup requires at least one stream");
    }
    if (options.count.has_value() && *options.count == 0) {
        throw std::invalid_argument("redis xreadgroup count must be greater than zero");
    }
    for (const auto& stream : streams) {
        if (stream.stream.empty() || stream.id.empty()) {
            throw std::invalid_argument("redis xreadgroup stream and id must not be empty");
        }
    }

    std::pmr::vector<std::pmr::string> args(resource);
    args.reserve(6 + streams.size() * 2 + (options.count.has_value() ? 2 : 0) + (options.block.has_value() ? 2 : 0) + (options.noAck ? 1 : 0));
    emplaceRedisString(args, "XREADGROUP");
    emplaceRedisString(args, "GROUP");
    emplaceRedisString(args, group);
    emplaceRedisString(args, consumer);
    if (options.count.has_value()) {
        emplaceRedisString(args, "COUNT");
        std::pmr::string count(resource);
        appendRedisNumber(count, *options.count);
        args.emplace_back(std::move(count));
    }
    if (options.block.has_value()) {
        emplaceRedisString(args, "BLOCK");
        if (const auto duration = options.block->duration(); duration.has_value()) {
            args.emplace_back(redisMillisecondsString(*duration, resource));
        } else {
            emplaceRedisString(args, "0");
        }
    }
    if (options.noAck) {
        emplaceRedisString(args, "NOACK");
    }
    emplaceRedisString(args, "STREAMS");
    for (const auto& stream : streams) {
        emplaceRedisString(args, stream.stream.view());
    }
    for (const auto& stream : streams) {
        emplaceRedisString(args, stream.id.view());
    }
    return args;
}

}  // namespace ruvia::detail
