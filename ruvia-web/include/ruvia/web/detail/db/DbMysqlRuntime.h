#pragma once

#include <mysql/mysql.h>

#include <chrono>
#include <optional>

namespace ruvia::detail {

void ensureMysqlThreadInitialized();
void setMysqlTimeout(
    st_mysql& connection,
    mysql_option option,
    std::optional<std::chrono::milliseconds> timeout) noexcept;

}  // namespace ruvia::detail
