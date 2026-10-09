#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db.h"

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.

namespace ruvia {

struct prepared_db_statement final {
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
};

[[nodiscard]] prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource);

}  // namespace ruvia
