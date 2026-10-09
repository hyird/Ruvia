#pragma once

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"

namespace ruvia::detail {

// The outer scoped operation owns both the driver task and the mapped result.
// Decoding finishes before db_rows releases its backend storage; the mapper
// must construct owning fields in the same operation memory domain.
template <typename result_type, typename mapper_type>
task<result_type> map_db_query(task<db_rows> query, std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    co_return mapper(std::move(rows), resource);
}

inline task<std::pair<db_rows, db_rows>> query_db_pair(task<db_rows> first, task<db_rows> second) {
    auto rows = co_await std::move(first);
    auto count = co_await std::move(second);
    co_return std::pair{std::move(rows), std::move(count)};
}

inline std::uint64_t db_count_value(const db_rows& rows) {
    if (rows.size() != 1) {
        throw std::runtime_error("database count did not return exactly one row");
    }
    return rows.front()["count"].as<std::uint64_t>().value();
}

template <typename result_type, typename mapper_type>
task<std::pair<result_type, std::uint64_t>> map_db_query_and_count(task<std::pair<db_rows, db_rows>> query,
    std::pmr::memory_resource* resource, mapper_type mapper) {
    auto rows = co_await std::move(query);
    const auto count = db_count_value(rows.second);
    co_return std::pair{mapper(std::move(rows.first), resource), count};
}

}  // namespace ruvia::detail
