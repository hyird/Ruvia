#pragma once

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"

namespace ruvia::detail {

// The outer scoped operation owns both the driver task and the mapped result.
// Decoding finishes before db_rows releases its backend storage; the mapper
// must construct owning fields in the same operation memory domain.
template <typename result_type, typename mapper_type>
task<result_type> map_db_query(
    operation_scope&, task<db_rows> query, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    co_return mapper(std::move(rows), resource);
}

inline std::uint64_t db_count_value(const db_rows& rows) {
    if (rows.size() != 1) {
        throw db_conversion_error(
            db_conversion_error::code_type::unexpected_row_count, "database count did not return exactly one row");
    }
    const auto count = rows.front()["count"].as<std::uint64_t>();
    if (!count) {
        throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "database count column is NULL");
    }
    return *count;
}

template <typename result_type, typename mapper_type>
task<std::pair<result_type, std::uint64_t>> map_db_query_and_count(operation_scope&,
    task<std::pair<db_rows, db_rows>> query, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    const auto count = db_count_value(rows.second);
    co_return std::pair{mapper(std::move(rows.first), resource), count};
}

}  // namespace ruvia::detail
