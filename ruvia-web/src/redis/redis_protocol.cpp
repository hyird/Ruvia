#include "redis/redis_protocol.h"

#include <hiredis/hiredis.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ruvia/web/detail/redis/redis_utils.h"

#include "redis/redis_types_access.h"

namespace ruvia::detail {
namespace {

template <typename args_type>
[[nodiscard]] std::size_t resp_serialized_size(const args_type& args) {
    const auto decimal_size = [](std::size_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max()) {
            throw std::length_error("redis RESP length is too large");
        }
        return unsigned_decimal_size(static_cast<std::uint64_t>(value));
    };
    const auto add_size = [](std::size_t& current, std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - current) {
            throw std::length_error("redis RESP command is too large");
        }
        current += amount;
    };

    std::size_t size = 0;
    add_size(size, 1);
    add_size(size, decimal_size(args.size()));
    add_size(size, 2);
    for (const auto& arg : args) {
        const auto bytes_value = static_cast<std::size_t>(arg.size());
        add_size(size, 1);
        add_size(size, decimal_size(bytes_value));
        add_size(size, 2);
        add_size(size, bytes_value);
        add_size(size, 2);
    }
    return size;
}

// Writes the RESP multi-bulk encoding of `args` to `output`. No manual
// reserve: `output` is the per-connection write buffer reused across requests,
// so in steady state it already holds enough capacity, and std::pmr::string's
// geometric growth keeps pipelined appends amortized O(total) bytes (an exact
// per-command reserve would instead force O(n^2) copying for large pipelines).
template <typename args_type>
void serialize_resp_command(std::pmr::string& output, const args_type& args, std::size_t serialized_size) {
    if (serialized_size > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.push_back('*');
    append_redis_number(output, static_cast<std::uint64_t>(args.size()));
    output.append("\r\n", 2);
    for (const auto& arg : args) {
        const auto size = static_cast<std::size_t>(arg.size());
        output.push_back('$');
        append_redis_number(output, static_cast<std::uint64_t>(size));
        output.append("\r\n", 2);
        if (size != 0) {
            output.append(arg.data(), size);
        }
        output.append("\r\n", 2);
    }
}

[[nodiscard]] std::string_view redis_reply_string_view(const redisReply& reply) {
    if (reply.str == nullptr) {
        if (reply.len != 0) {
            throw redis_error(redis_error::code_type::protocol_error, "invalid redis string reply");
        }
        return {};
    }
    return std::string_view(reply.str, reply.len);
}

}  // namespace

void append_resp_command(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serialize_resp_command(output, args, resp_serialized_size(args));
}

void append_resp_command(std::pmr::string& output, std::span<const std::pmr::string> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serialize_resp_command(output, args, resp_serialized_size(args));
}

std::size_t resp_command_serialized_size(std::span<const std::string_view> args) {
    return resp_serialized_size(args);
}

std::size_t resp_command_serialized_size(std::span<const std::pmr::string> args) {
    return resp_serialized_size(args);
}

redis_value hiredis_reply_to_value(const redisReply& reply, std::size_t depth, std::size_t max_depth,
    std::pmr::memory_resource* resource) {
    if (max_depth > 0 && depth > max_depth) {
        throw redis_error(redis_error::code_type::protocol_error, "redis array nesting is too deep");
    }

    switch (reply.type) {
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
#ifdef REDIS_REPLY_BIGNUM
        case REDIS_REPLY_BIGNUM:
#endif
#ifdef REDIS_REPLY_VERB
        case REDIS_REPLY_VERB:
#endif
            return redis_types_access::string_value(redis_reply_string_view(reply), resource);
        case REDIS_REPLY_ERROR:
            return redis_types_access::error_value(redis_reply_string_view(reply), resource);
        case REDIS_REPLY_INTEGER:
            return redis_types_access::integer_value(
                static_cast<std::int64_t>(reply.integer), resource);
        case REDIS_REPLY_NIL:
            return redis_types_access::null_value(resource);
#ifdef REDIS_REPLY_DOUBLE
        case REDIS_REPLY_DOUBLE:
            return redis_types_access::string_value(redis_reply_string_view(reply), resource);
#endif
#ifdef REDIS_REPLY_BOOL
        case REDIS_REPLY_BOOL:
            return redis_types_access::integer_value(reply.integer == 0 ? 0 : 1, resource);
#endif
        case REDIS_REPLY_ARRAY:
#ifdef REDIS_REPLY_MAP
        case REDIS_REPLY_MAP:
#endif
#ifdef REDIS_REPLY_SET
        case REDIS_REPLY_SET:
#endif
#ifdef REDIS_REPLY_ATTR
        case REDIS_REPLY_ATTR:
#endif
#ifdef REDIS_REPLY_PUSH
        case REDIS_REPLY_PUSH:
#endif
        {
            if (max_depth > 0 && depth >= max_depth) {
                throw redis_error(
                    redis_error::code_type::protocol_error, "redis array nesting is too deep");
            }
            std::pmr::vector<redis_value> values(resource);
            values.reserve(reply.elements);
            for (std::size_t i = 0; i < reply.elements; ++i) {
                if (reply.element == nullptr || reply.element[i] == nullptr) {
                    throw redis_error(redis_error::code_type::protocol_error, "invalid redis array reply");
                }
                values.emplace_back(
                    hiredis_reply_to_value(*reply.element[i], depth + 1, max_depth, resource));
            }
            return redis_types_access::array_value(std::move(values), resource);
        }
        default:
            throw redis_error(redis_error::code_type::protocol_error, "unsupported redis reply type");
    }
}

const char* hiredis_reader_error(const redisReader& reader_value) noexcept {
    return reader_value.errstr[0] == '\0' ? "redis protocol error" : reader_value.errstr;
}

}  // namespace ruvia::detail
