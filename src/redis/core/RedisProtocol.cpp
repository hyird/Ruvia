#include "RedisProtocol.h"

#include "RedisUtils.h"

#include <hiredis/hiredis.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ruvia::detail {
namespace {

// Writes the RESP multi-bulk encoding of `args` to `output`. No manual
// reserve: `output` is the per-connection write buffer reused across requests,
// so in steady state it already holds enough capacity, and std::pmr::string's
// geometric growth keeps pipelined appends amortized O(total) bytes (an exact
// per-command reserve would instead force O(n^2) copying for large pipelines).
template <typename Args>
void serializeRespCommand(std::pmr::string& output, const Args& args) {
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

}  // namespace

void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serializeRespCommand(output, args);
}

void appendRespCommand(std::pmr::string& output, std::span<const std::pmr::string> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    serializeRespCommand(output, args);
}

RedisValue hiredisReplyToValue(
    const redisReply& reply,
    std::size_t depth,
    std::size_t maxDepth,
    std::pmr::memory_resource* resource) {
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
            return RedisValue::stringValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
        case REDIS_REPLY_ERROR:
            return RedisValue::errorValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
        case REDIS_REPLY_INTEGER:
            return RedisValue::integerValue(static_cast<std::int64_t>(reply.integer), resource);
        case REDIS_REPLY_NIL:
            return RedisValue::nullValue(resource);
#ifdef REDIS_REPLY_DOUBLE
        case REDIS_REPLY_DOUBLE:
            return RedisValue::stringValue(std::string_view(reply.str == nullptr ? "" : reply.str, reply.len), resource);
#endif
#ifdef REDIS_REPLY_BOOL
        case REDIS_REPLY_BOOL:
            return RedisValue::integerValue(reply.integer == 0 ? 0 : 1, resource);
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
            return RedisValue::arrayValue(std::move(values), resource);
        }
        default:
            throw RedisError(RedisError::Code::kProtocolError, "unsupported redis reply type");
    }
}

const char* hiredisReaderError(const redisReader& reader) noexcept {
    return reader.errstr[0] == '\0' ? "redis protocol error" : reader.errstr;
}

}  // namespace ruvia::detail
