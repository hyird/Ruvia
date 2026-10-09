#pragma once

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_types.h"

struct pg_conn;
struct pg_result;

namespace ruvia::detail {

[[nodiscard]] db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type code, const pg_result* result = nullptr);

struct postgresql_params final {
    explicit postgresql_params(std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> encoded_;
    std::pmr::vector<const char*> values_;
    std::pmr::vector<int> lengths_;
};

[[nodiscard]] postgresql_params encode_postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource);

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

}  // namespace ruvia::detail
