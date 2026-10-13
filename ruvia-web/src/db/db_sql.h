#pragma once

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/db/db.h"

struct st_mysql;

namespace ruvia::detail {

// Builds the error for a failed driver call. A statement_failed whose cause
// is a lost connection is reported as io_error.
[[nodiscard]] db_error mysql_error(
    const st_mysql& connection, std::string_view operation, db_error::code_type code);

// MariaDB's C API carries SQL and escaped-value lengths in unsigned long.
// Reject a wider C++ view before any narrowing conversion reaches the driver.
void validate_mariadb_sql_length(std::size_t length);

void free_stored_result(void* result_value) noexcept;

[[nodiscard]] std::pmr::string interpolate_sql(st_mysql& connection, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
