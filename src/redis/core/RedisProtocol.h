#pragma once

#include "ruvia/redis/Redis.h"

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

struct redisReader;
struct redisReply;

namespace ruvia::detail {

// Serializes a command into RESP multi-bulk form, appending directly to the
// reused connection write buffer. Both overloads avoid the extra heap
// allocation + copy that hiredis' redisFormatCommandArgv would impose; the
// pmr::string overload also lets owned-argument paths skip building an
// intermediate string_view vector.
void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args);
void appendRespCommand(std::pmr::string& output, std::span<const std::pmr::string> args);
[[nodiscard]] RedisValue hiredisReplyToValue(
    const redisReply& reply,
    std::size_t depth,
    std::size_t maxDepth,
    std::pmr::memory_resource* resource);
[[nodiscard]] const char* hiredisReaderError(const redisReader& reader) noexcept;

}  // namespace ruvia::detail
