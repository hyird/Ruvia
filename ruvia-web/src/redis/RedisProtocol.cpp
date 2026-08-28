#include "ruvia/web/detail/redis/RedisProtocol.h"

#include "ruvia/web/detail/redis/RedisTypesAccess.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <hiredis/hiredis.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {
namespace {

template <typename Args>
[[nodiscard]] std::size_t respSerializedSize(const Args& args) {
    const auto decimalSize = [](std::size_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max()) {
            throw std::length_error("redis RESP length is too large");
        }
        return httpUnsignedDecimalSize(static_cast<std::uint64_t>(value));
    };
    const auto addSize = [](std::size_t& current, std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - current) {
            throw std::length_error("redis RESP command is too large");
        }
        current += amount;
    };

    std::size_t size = 0;
    addSize(size, 1);
    addSize(size, decimalSize(args.size()));
    addSize(size, 2);
    for (const auto& arg : args) {
        const auto bytes = static_cast<std::size_t>(arg.size());
        addSize(size, 1);
        addSize(size, decimalSize(bytes));
        addSize(size, 2);
        addSize(size, bytes);
        addSize(size, 2);
    }
    return size;
}

// Writes the RESP multi-bulk encoding of `args` to `output`. No manual
// reserve: `output` is the per-connection write buffer reused across requests,
// so in steady state it already holds enough capacity, and std::pmr::string's
// geometric growth keeps pipelined appends amortized O(total) bytes (an exact
// per-command reserve would instead force O(n^2) copying for large pipelines).
template <typename Args>
void serializeRespCommand(std::pmr::string& output, const Args& args, std::size_t serializedSize) {
    if (serializedSize > std::numeric_limits<std::size_t>::max() - output.size()) {
        throw std::length_error("redis RESP output is too large");
    }
    output.push_back('*');
    appendRedisNumber(output, static_cast<std::uint64_t>(args.size()));
    output.append("\r\n", 2);
    for (const auto& arg : args) {
        const auto size = static_cast<std::size_t>(arg.size());
        output.push_back('$');
        appendRedisNumber(output, static_cast<std::uint64_t>(size));
        output.append("\r\n", 2);
        if (size != 0) {
            output.append(arg.data(), size);
        }
        output.append("\r\n", 2);
    }
}

[[nodiscard]] std::string_view redisReplyStringView(const redisReply& reply) {
    if (reply.str == nullptr) {
        if (reply.len != 0) {
            throw RedisError(RedisError::Code::kProtocolError, "invalid redis string reply");
        }
        return {};
    }
    return std::string_view(reply.str, reply.len);
}

}  // namespace

void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serializeRespCommand(output, args, respSerializedSize(args));
}

void appendRespCommand(std::pmr::string& output, std::span<const std::pmr::string> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serializeRespCommand(output, args, respSerializedSize(args));
}

std::size_t respCommandSerializedSize(std::span<const std::string_view> args) {
    return respSerializedSize(args);
}

std::size_t respCommandSerializedSize(std::span<const std::pmr::string> args) {
    return respSerializedSize(args);
}

RedisValue hiredisReplyToValue(const redisReply& reply, std::size_t depth, std::size_t maxDepth, std::pmr::memory_resource* resource) {
    if (maxDepth > 0 && depth > maxDepth) {
        throw RedisError(RedisError::Code::kProtocolError, "redis array nesting is too deep");
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
            return RedisTypesAccess::stringValue(redisReplyStringView(reply), resource);
        case REDIS_REPLY_ERROR:
            return RedisTypesAccess::errorValue(redisReplyStringView(reply), resource);
        case REDIS_REPLY_INTEGER:
            return RedisTypesAccess::integerValue(static_cast<std::int64_t>(reply.integer), resource);
        case REDIS_REPLY_NIL:
            return RedisTypesAccess::nullValue(resource);
#ifdef REDIS_REPLY_DOUBLE
        case REDIS_REPLY_DOUBLE:
            return RedisTypesAccess::stringValue(redisReplyStringView(reply), resource);
#endif
#ifdef REDIS_REPLY_BOOL
        case REDIS_REPLY_BOOL:
            return RedisTypesAccess::integerValue(reply.integer == 0 ? 0 : 1, resource);
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
            if (maxDepth > 0 && depth >= maxDepth) {
                throw RedisError(RedisError::Code::kProtocolError, "redis array nesting is too deep");
            }
            std::pmr::vector<RedisValue> values(resource);
            values.reserve(reply.elements);
            for (std::size_t i = 0; i < reply.elements; ++i) {
                if (reply.element == nullptr || reply.element[i] == nullptr) {
                    throw RedisError(RedisError::Code::kProtocolError, "invalid redis array reply");
                }
                values.emplace_back(hiredisReplyToValue(*reply.element[i], depth + 1, maxDepth, resource));
            }
            return RedisTypesAccess::arrayValue(std::move(values), resource);
        }
        default:
            throw RedisError(RedisError::Code::kProtocolError, "unsupported redis reply type");
    }
}

const char* hiredisReaderError(const redisReader& reader) noexcept {
    return reader.errstr[0] == '\0' ? "redis protocol error" : reader.errstr;
}

}  // namespace ruvia::detail
