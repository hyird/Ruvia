#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/web/redis/redis.h"

struct redisReader;
struct redisReply;

namespace ruvia::detail {

// Serializes a command into RESP multi-bulk form, appending directly to the
// reused connection write buffer. Both overloads avoid the extra heap
// allocation + copy that hiredis' redisFormatCommandArgv would impose; the
// pmr::string overload also lets owned-argument paths skip building an
// intermediate string_view vector.
void append_resp_command(std::pmr::string& output, std::span<const std::string_view> args);
void append_resp_command(std::pmr::string& output, std::span<const std::pmr::string> args);
// Throws std::length_error when the complete RESP command size cannot be
// represented by std::size_t.
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::string_view> args);
[[nodiscard]] std::size_t resp_command_serialized_size(std::span<const std::pmr::string> args);
[[nodiscard]] redis_value hiredis_reply_to_value(const redisReply& reply, std::size_t depth,
    std::size_t max_depth, std::pmr::memory_resource* resource);
[[nodiscard]] const char* hiredis_reader_error(const redisReader& reader_value) noexcept;

}  // namespace ruvia::detail
