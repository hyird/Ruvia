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

[[nodiscard]] std::runtime_error mysqlError(const st_mysql& connection, std::string_view operation);

void freeStoredResult(st_mysql_res* result) noexcept;

[[nodiscard]] std::pmr::string interpolateSql(
    st_mysql& connection,
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
