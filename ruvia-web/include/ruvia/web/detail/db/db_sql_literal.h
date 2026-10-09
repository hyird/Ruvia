#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_sql_scan.h"
#include "ruvia/web/fixed_string.h"

namespace ruvia::detail {

struct db_sql_parameter_count {
    std::size_t count_{0};
    bool valid_{true};
};

// Count MariaDB slots, or PostgreSQL's highest numbered parameter. Repeated
// PostgreSQL references bind the same value; '?' remains a PostgreSQL operator.
// This checks the binding contract, not SQL grammar or schema correctness.
[[nodiscard]] constexpr db_sql_parameter_count count_db_sql_parameters(std::string_view sql, db_driver driver) noexcept {
    db_sql_parameter_count result;
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        return {0, false};
    }
    for (std::size_t index = 0; index < sql.size();) {
        if (sql[index] == '\0') {
            return {0, false};
        }
        const auto next_value = driver == db_driver::mariadb ? skip_sql_atom(sql, index) : skip_postgresql_sql_atom(sql, index);
        if (next_value != index + 1) {
            index = next_value;
            continue;
        }
        if (driver == db_driver::mariadb) {
            result.count_ += sql[index] == '?';
            ++index;
            continue;
        }
        if (sql[index] == '$' && index + 1 < sql.size() && sql[index + 1] >= '0' && sql[index + 1] <= '9') {
            std::size_t number = 0;
            ++index;
            while (index < sql.size() && sql[index] >= '0' && sql[index] <= '9') {
                const auto digit = static_cast<unsigned>(sql[index++] - '0');
                if (number > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) {
                    return {0, false};
                }
                number = number * 10 + digit;
            }
            if (number == 0) {
                return {0, false};
            }
            if (number > result.count_) {
                result.count_ = number;
            }
            continue;
        }
        // Unquoted identifiers can contain '$'; account$1 is not a parameter.
        if (is_postgresql_dollar_tag_start(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80) {
            do {
                ++index;
            } while (index < sql.size() && (is_postgresql_identifier_continue(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80));
        } else {
            ++index;
        }
    }
    return result;
}

template <fixed_string sql, db_driver driver, std::size_t parameter_count>
void validate_db_sql_literal(db_driver actual_driver) {
    static_assert(driver == db_driver::mariadb || driver == db_driver::postgresql,
        "SQL literal requires a concrete database driver");
    static_assert(sql.view().find('\0') == std::string_view::npos, "SQL literal must not contain NUL bytes");
    constexpr auto result_value = count_db_sql_parameters(sql.view(), driver);
    static_assert(result_value.valid_, "SQL literal contains an invalid parameter index");
    static_assert(result_value.count_ == parameter_count, "SQL placeholder count does not match the number of arguments");
    if (actual_driver != driver) {
        throw std::invalid_argument("SQL literal dialect does not match the database driver");
    }
}

}  // namespace ruvia::detail
