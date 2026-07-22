#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/web/db/Db.h"

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.

namespace ruvia {

struct PreparedDbStatement final {
    std::pmr::string sql;
    std::pmr::vector<DbValue> params;
};

[[nodiscard]] PreparedDbStatement prepareDbStatement(
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource);

}  // namespace ruvia
