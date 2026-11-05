#include "db/db_prepared_statement.h"

#include <array>
#include <stdexcept>

#include "ruvia/web/detail/db/db_sql_scan.h"

#include "db/db_value_clone.h"

namespace ruvia {

void validate_db_sql_text(std::string_view sql) {
    if (!detail::has_sql_non_whitespace(sql)) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
}

prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource) {
    validate_db_sql_text(sql);
    auto* resolved = pmr_resource_or_default(resource);
    return prepared_db_statement{std::pmr::string(sql, resolved), detail::clone_db_values(params, resolved)};
}

prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload, std::pmr::memory_resource* resource) {
    if (channel.empty()) {
        throw std::invalid_argument("database notify channel must not be empty");
    }
    const std::array<db_value, 2> params{db_value(channel), db_value(payload)};
    return prepare_db_statement("SELECT \"pg_notify\"($1::text, $2::text)", params, resource);
}

}  // namespace ruvia
