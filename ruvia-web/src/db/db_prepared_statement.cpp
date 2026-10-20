#include "db/db_prepared_statement.h"

#include <stdexcept>

#include "ruvia/web/detail/db/db_sql_scan.h"

#include "db/db_pool_utils.h"
namespace ruvia {

prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource) {
    if (!detail::has_sql_non_whitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    for (const auto& param : params) {
        if (detail::db_value_access::type(param) == detail::db_value_type::string &&
            detail::db_value_access::text(param).find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database string parameter must not contain NUL bytes");
        }
    }
    auto* resolved = pmr_resource_or_default(resource);
    return prepared_db_statement{
        std::pmr::string(sql, resolved), detail::clone_db_values(params, resolved)};
}

}  // namespace ruvia
