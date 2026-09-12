#include "ruvia/web/detail/redis/RedisRepositoryCommands.h"

#include <stdexcept>

#include "ruvia/web/detail/redis/RedisHandleHelpers.h"

namespace ruvia::detail {
namespace {

// ARGV: mode, ttl policy, milliseconds, required count, required names,
// followed by mutation triples (set/delete, field, value). All validation
// precedes writes: Redis scripts are atomic but do not roll back runtime errors.
constexpr std::string_view kWriteScript = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
local exists = kind ~= 'none'
if exists and kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if exists and redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
if ARGV[1] == 'insert' and exists then return 0 end
if ARGV[1] == 'update' and not exists then return 0 end
local required = tonumber(ARGV[4])
local start = 5 + required
local changes = {}
for i = start, #ARGV, 3 do changes[ARGV[i + 1]] = ARGV[i] end
for i = 5, 4 + required do
    local field = ARGV[i]
    if changes[field] == 'd' or
       (changes[field] ~= 's' and (not exists or redis.call('HEXISTS', KEYS[1], field) == 0)) then
        return redis.error_reply('required entity field is missing: ' .. field)
    end
end
for i = start, #ARGV, 3 do
    if ARGV[i] == 's' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2])
    else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end
end
redis.call('HSET', KEYS[1], '__ruvia_entity', '1')
if ARGV[2] == 'expire' then redis.call('PEXPIRE', KEYS[1], ARGV[3])
elseif ARGV[2] == 'persist' then redis.call('PERSIST', KEYS[1]) end
if exists then return 2 else return 1 end
)lua";

constexpr std::string_view kDeleteScript = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return 0 end
if kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
return redis.call('DEL', KEYS[1])
)lua";

}  // namespace

std::pmr::vector<std::string_view> redisOrmArgumentViews(const RedisOrmArguments& args) {
    std::pmr::vector<std::string_view> views(args.get_allocator().resource());
    views.reserve(args.size());
    for (const auto& arg : args) {
        views.emplace_back(arg);
    }
    return views;
}

RedisOrmArguments redisOrmWriteArguments(std::string_view key, std::string_view mode,
    RedisWriteOptions options, std::pmr::memory_resource* resource) {
    if (options.ttl && (options.persist || options.ttl->count() <= 0 || options.ttl->count() > 9007199254740991LL)) {
        throw std::invalid_argument("entity TTL must be positive, at most 2^53-1 milliseconds, and cannot combine with persist");
    }
    RedisOrmArguments args(resource);
    args.emplace_back("EVAL");
    args.emplace_back(kWriteScript);
    args.emplace_back("1");
    args.emplace_back(key);
    args.emplace_back(mode);
    args.emplace_back(options.ttl ? "expire" : options.persist ? "persist"
                                                               : "keep");
    args.push_back(redisMillisecondsString(options.ttl.value_or(std::chrono::milliseconds(0)), resource));
    return args;
}

RedisOrmArguments redisOrmDeleteArguments(std::string_view key, std::pmr::memory_resource* resource) {
    RedisOrmArguments args(resource);
    args.emplace_back("EVAL");
    args.emplace_back(kDeleteScript);
    args.emplace_back("1");
    args.emplace_back(key);
    return args;
}

DbExecResult redisOrmExecResult(RedisValue&& reply, std::pmr::memory_resource*) {
    const auto value = redisValueInteger(reply);
    switch (value) {
        case 0:
            return RedisOrmResultAccess::makeExecResult(0);
        case 1:
        case 2:
            return RedisOrmResultAccess::makeExecResult(1);
        default:
            throw RedisError(RedisError::Code::kProtocolError,
                "invalid entity write affected-row reply");
    }
}

DbExecResult redisOrmDeleteResult(RedisValue&& reply, std::pmr::memory_resource*) {
    const auto value = redisValueInteger(reply);
    if (value == 0 || value == 1) {
        return RedisOrmResultAccess::makeExecResult(static_cast<std::uint64_t>(value));
    }
    throw RedisError(RedisError::Code::kProtocolError,
        "invalid entity delete affected-row reply");
}

bool redisOrmBooleanResult(RedisValue&& reply, std::pmr::memory_resource*) {
    return redisValueIntegerBool(reply);
}

void redisOrmStatusResult(RedisValue&& reply, std::pmr::memory_resource*) {
    if (redisValueString(reply) != "OK") {
        throw RedisError(RedisError::Code::kProtocolError, "invalid entity index status reply");
    }
}

std::span<const RedisValue> redisOrmArray(const RedisValue& reply) {
    return redisValueArray(reply);
}
std::string_view redisOrmString(const RedisValue& reply) {
    return redisValueString(reply);
}
std::uint64_t redisOrmCount(const RedisValue& reply) {
    const auto count = redisValueInteger(reply);
    if (count < 0) {
        throw RedisError(RedisError::Code::kProtocolError, "negative entity result count");
    }
    return static_cast<std::uint64_t>(count);
}

}  // namespace ruvia::detail
