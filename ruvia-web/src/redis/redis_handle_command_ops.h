#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include "ruvia/web/redis/redis.h"

#include "redis/redis_registry.h"

namespace ruvia::detail {

task<void> execute_redis_ping(redis_command_executor executor, std::pmr::vector<std::pmr::string> args,
    std::pmr::memory_resource* resource);

task<redis_set_result> execute_redis_set(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, redis_set_options options,
    std::pmr::memory_resource* resource);

task<bool> execute_redis_integer_bool(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

task<std::pmr::vector<redis_key_value>> execute_redis_key_value_array(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context_value,
    std::pmr::memory_resource* resource);

task<std::pmr::vector<redis_scored_value>> execute_redis_scored_array(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::pmr::memory_resource* resource);

task<std::optional<double>> execute_redis_optional_double(redis_command_executor executor,
    std::pmr::vector<std::pmr::string> args, std::string_view context_value,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
