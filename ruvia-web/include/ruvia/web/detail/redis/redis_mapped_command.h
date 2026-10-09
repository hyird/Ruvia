#pragma once

#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

// One scoped operation owns the command and its decoding. Mappers must return
// owning values in the handle's reclaimable worker memory domain.
template <typename result_type, typename mapper_type>
task<result_type> map_redis_command(task<redis_value> command, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto reply = co_await std::move(command);
    if constexpr (std::is_void_v<result_type>) {
        mapper(std::move(reply), resource);
    } else {
        co_return mapper(std::move(reply), resource);
    }
}

}  // namespace ruvia::detail
