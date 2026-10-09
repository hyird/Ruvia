#include "ruvia/web/detail/redis/redis_repository_commands.h"

#include <stdexcept>

#include "redis/redis_handle_helpers.h"

namespace ruvia::detail {
namespace {

// ARGV: mode, ttl policy, milliseconds, required count, required names,
// followed by mutation triples (set/delete, field, value). All validation
// precedes writes: Redis scripts are atomic but do not roll back runtime errors.
constexpr std::string_view write_script = R"lua(
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

constexpr std::string_view delete_script = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return 0 end
if kind ~= 'hash' then return redis.error_reply('WRONGTYPE entity key is not a hash') end
if redis.call('HGET', KEYS[1], '__ruvia_entity') ~= '1' then
    return redis.error_reply('entity key is not owned by this repository')
end
return redis.call('DEL', KEYS[1])
)lua";

}  // namespace

std::pmr::vector<std::string_view> redis_orm_argument_views(const redis_orm_arguments_type& args) {
    std::pmr::vector<std::string_view> views(args.get_allocator().resource());
    views.reserve(args.size());
    for (const auto& arg : args) {
        views.emplace_back(arg);
    }
    return views;
}

redis_orm_arguments_type redis_orm_write_arguments(std::string_view key, std::string_view mode,
    redis_write_options options, std::pmr::memory_resource* resource) {
    if (options.ttl_ && (options.persist_ || options.ttl_->count() <= 0 || options.ttl_->count() > 9007199254740991LL)) {
        throw std::invalid_argument("entity TTL must be positive, at most 2^53-1 milliseconds, and cannot combine with persist");
    }
    redis_orm_arguments_type args(resource);
    args.emplace_back("EVAL");
    args.emplace_back(write_script);
    args.emplace_back("1");
    args.emplace_back(key);
    args.emplace_back(mode);
    args.emplace_back(options.ttl_ ? "expire" : options.persist_ ? "persist"
                                                                 : "keep");
    args.push_back(redis_milliseconds_string(options.ttl_.value_or(std::chrono::milliseconds(0)), resource));
    return args;
}

redis_orm_arguments_type redis_orm_delete_arguments(std::string_view key, std::pmr::memory_resource* resource) {
    redis_orm_arguments_type args(resource);
    args.emplace_back("EVAL");
    args.emplace_back(delete_script);
    args.emplace_back("1");
    args.emplace_back(key);
    return args;
}

redis_write_result redis_orm_exec_result(redis_value&& reply, std::pmr::memory_resource*) {
    const auto value = redis_value_integer(reply);
    switch (value) {
        case 0:
            return redis_write_result(0);
        case 1:
        case 2:
            return redis_write_result(1);
        default:
            throw redis_error(redis_error::code_type::protocol_error,
                "invalid entity write affected-row reply");
    }
}

redis_write_result redis_orm_delete_result(redis_value&& reply, std::pmr::memory_resource*) {
    const auto value = redis_value_integer(reply);
    if (value == 0 || value == 1) {
        return redis_write_result(static_cast<std::uint64_t>(value));
    }
    throw redis_error(redis_error::code_type::protocol_error,
        "invalid entity delete affected-row reply");
}

bool redis_orm_boolean_result(redis_value&& reply, std::pmr::memory_resource*) {
    return redis_value_integer_bool(reply);
}

void redis_orm_status_result(redis_value&& reply, std::pmr::memory_resource*) {
    if (redis_value_string(reply) != "OK") {
        throw redis_error(redis_error::code_type::protocol_error, "invalid entity index status reply");
    }
}

std::span<const redis_value> redis_orm_array(const redis_value& reply) {
    return redis_value_array(reply);
}
std::string_view redis_orm_string(const redis_value& reply) {
    return redis_value_string(reply);
}
std::uint64_t redis_orm_count(const redis_value& reply) {
    const auto count = redis_value_integer(reply);
    if (count < 0) {
        throw redis_error(redis_error::code_type::protocol_error, "negative entity result count");
    }
    return static_cast<std::uint64_t>(count);
}

}  // namespace ruvia::detail
