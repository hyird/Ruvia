#include "RedisProtocol.h"

#include <hiredis/hiredis.h>

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ruvia::detail {

void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args) {
    if (args.empty()) {
        throw std::invalid_argument("redis command must not be empty");
    }
    if (args.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("redis command has too many arguments");
    }

    constexpr std::size_t stackArgLimit = 32;
    std::array<const char*, stackArgLimit> stackArgv{};
    std::array<std::size_t, stackArgLimit> stackArgvLen{};
    std::pmr::vector<const char*> heapArgv(output.get_allocator().resource());
    std::pmr::vector<std::size_t> heapArgvLen(output.get_allocator().resource());

    const bool useStackArgs = args.size() <= stackArgLimit;
    auto* argv = stackArgv.data();
    auto* argvLen = stackArgvLen.data();
    if (!useStackArgs) {
        heapArgv.reserve(args.size());
        heapArgvLen.reserve(args.size());
        argv = heapArgv.data();
        argvLen = heapArgvLen.data();
    }

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (useStackArgs) {
            stackArgv[i] = args[i].data();
            stackArgvLen[i] = args[i].size();
        } else {
            heapArgv.push_back(args[i].data());
            heapArgvLen.push_back(args[i].size());
        }
    }
    if (!useStackArgs) {
        argv = heapArgv.data();
        argvLen = heapArgvLen.data();
    }

    char* command = nullptr;
    const auto size = redisFormatCommandArgv(
        &command,
        static_cast<int>(args.size()),
        argv,
        argvLen);
    if (size < 0 || command == nullptr) {
        throw RedisError(RedisError::Code::kProtocolError, "failed to format redis command");
    }
    output.append(command, static_cast<std::size_t>(size));
    redisFreeCommand(command);
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
