#pragma once

#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

// One scoped operation owns the command and its decoding. Mappers must return
// owning values in the handle's reclaimable worker memory domain.
template <typename Result, typename Mapper>
Task<Result> mapRedisCommand(Task<RedisValue> command, std::pmr::memory_resource* resource, Mapper mapper) {
    auto reply = co_await std::move(command);
    if constexpr (std::is_void_v<Result>) {
        mapper(std::move(reply), resource);
    } else {
        co_return mapper(std::move(reply), resource);
    }
}

}  // namespace ruvia::detail
