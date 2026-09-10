#pragma once

#include <cstdint>
#include <memory_resource>
#include <stdexcept>
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

inline Task<std::pair<DbRows, DbRows>> queryDbPair(Task<DbRows> first, Task<DbRows> second) {
    auto rows = co_await std::move(first);
    auto count = co_await std::move(second);
    co_return std::pair{std::move(rows), std::move(count)};
}

inline std::uint64_t dbCountValue(const DbRows& rows) {
    if (rows.size() != 1) {
        throw std::runtime_error("database count did not return exactly one row");
    }
    return rows.front()["count"].as<std::uint64_t>().value();
}

template <typename Result, typename Mapper>
Task<std::pair<Result, std::uint64_t>> mapDbQueryAndCount(Task<std::pair<DbRows, DbRows>> query,
    std::pmr::memory_resource* resource, Mapper mapper) {
    auto rows = co_await std::move(query);
    const auto count = dbCountValue(rows.second);
    co_return std::pair{mapper(std::move(rows.first), resource), count};
}

}  // namespace ruvia::detail
