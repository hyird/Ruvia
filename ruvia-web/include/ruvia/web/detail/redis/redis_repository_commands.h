#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/redis/redis_repository_types.h"
#include "ruvia/web/redis/redis_types.h"
#include "ruvia/web/redis/redis_write_result.h"

namespace ruvia::detail {

using redis_orm_arguments_type = std::pmr::vector<std::pmr::string>;

[[nodiscard]] redis_orm_arguments_type redis_orm_write_arguments(std::string_view key, std::string_view mode,
    redis_write_options options, std::pmr::memory_resource* resource);
[[nodiscard]] redis_orm_arguments_type redis_orm_delete_arguments(std::string_view key, std::pmr::memory_resource* resource);
[[nodiscard]] redis_write_result redis_orm_exec_result(redis_value&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] redis_write_result redis_orm_delete_result(redis_value&& reply, std::pmr::memory_resource* resource);
void redis_orm_status_result(redis_value&& reply, std::pmr::memory_resource* resource);
[[nodiscard]] std::span<const redis_value> redis_orm_array(const redis_value& reply);
[[nodiscard]] std::string_view redis_orm_string(const redis_value& reply);
[[nodiscard]] std::uint64_t redis_orm_count(const redis_value& reply);

}  // namespace ruvia::detail
