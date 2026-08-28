#pragma once

#include "ruvia/web/db/Db.h"

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

struct st_mysql;
struct st_mysql_res;

namespace ruvia::detail {

[[nodiscard]] DbError mysqlError(const st_mysql& connection, std::string_view operation, DbError::Code code);

// MariaDB's C API carries SQL and escaped-value lengths in unsigned long.
// Reject a wider C++ view before any narrowing conversion reaches the driver.
void validateMariaDbSqlLength(std::size_t length);

void freeStoredResult(void* result) noexcept;

[[nodiscard]] std::pmr::string interpolateSql(st_mysql& connection, std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
