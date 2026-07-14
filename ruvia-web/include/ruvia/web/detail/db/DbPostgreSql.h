#pragma once

#include "ruvia/web/db/DbTypes.h"

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct pg_conn;
struct pg_result;

namespace ruvia::detail {

[[nodiscard]] std::runtime_error postgreSqlError(
    const pg_conn& connection,
    std::string_view operation,
    const pg_result* result = nullptr);

struct PostgreSqlParams final {
    explicit PostgreSqlParams(std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> encoded;
    std::pmr::vector<const char*> values;
};

[[nodiscard]] PostgreSqlParams encodePostgreSqlParams(
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource);

[[nodiscard]] std::uint64_t postgreSqlAffectedRows(const pg_result& result) noexcept;

}  // namespace ruvia::detail
