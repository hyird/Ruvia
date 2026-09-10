#pragma once

#include <memory_resource>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/web/db/DbRows.h"

namespace ruvia::detail {

// The outer scoped operation owns both the driver task and the mapped result.
// Decoding finishes before DbRows releases its backend storage; the mapper
// must construct owning fields in the same operation memory domain.
template <typename Result, typename Mapper>
Task<Result> mapDbQuery(Task<DbRows> query, std::pmr::memory_resource* resource, Mapper mapper) {
    auto rows = co_await std::move(query);
    co_return mapper(std::move(rows), resource);
}

}  // namespace ruvia::detail
