#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_types.h"
#include "ruvia/web/redis/redis_write_result.h"

namespace ruvia::detail {

class redis_pool;

using redis_orm_arguments_type = std::pmr::vector<std::pmr::string>;

// An ORM Lua script and the lowercase hex SHA1 Redis caches it under.
struct redis_orm_script final {
    std::string_view body_;
    std::string_view sha1_;
};

[[nodiscard]] const redis_orm_script& redis_orm_write_script();
[[nodiscard]] const redis_orm_script& redis_orm_delete_script();

// Both argument lists start with EVALSHA and the script digest.
[[nodiscard]] redis_orm_arguments_type redis_orm_write_arguments(std::string_view key, std::string_view mode,
    redis_write_options options, std::pmr::memory_resource* resource);
[[nodiscard]] redis_orm_arguments_type redis_orm_delete_arguments(std::string_view key, std::pmr::memory_resource* resource);
// Runs an EVALSHA argument list for `script`; a NOSCRIPT reply loads the body
// through EVAL once within the same operation.
[[nodiscard]] task<redis_value> execute_redis_orm_script(redis_pool& pool, const redis_orm_script& script,
    redis_orm_arguments_type args, operation_options options, std::pmr::memory_resource* resource);
[[nodiscard]] redis_write_result redis_orm_exec_result(redis_value&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] redis_write_result redis_orm_delete_result(redis_value&& reply, std::pmr::memory_resource* resource);
void redis_orm_status_result(redis_value&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] std::span<const redis_value> redis_orm_array(const redis_value& reply);
[[nodiscard]] std::string_view redis_orm_string(const redis_value& reply);
[[nodiscard]] std::uint64_t redis_orm_count(const redis_value& reply);

}  // namespace ruvia::detail
