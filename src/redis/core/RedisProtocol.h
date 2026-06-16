#pragma once

#include "ruvia/redis/Redis.h"

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>

struct redisReader;
struct redisReply;

namespace ruvia::detail {

void appendRespCommand(std::pmr::string& output, std::span<const std::string_view> args);
[[nodiscard]] RedisValue hiredisReplyToValue(
    const redisReply& reply,
    std::size_t depth,
    std::size_t maxDepth,
    std::pmr::memory_resource* resource);
[[nodiscard]] const char* hiredisReaderError(const redisReader& reader) noexcept;

}  // namespace ruvia::detail
