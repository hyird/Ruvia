#pragma once

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/FixedString.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbSqlScan.h"

namespace ruvia::detail {

struct DbSqlParameterCount {
    std::size_t count{0};
    bool valid{true};
};

// Count MariaDB slots, or PostgreSQL's highest numbered parameter. Repeated
// PostgreSQL references bind the same value; '?' remains a PostgreSQL operator.
// This checks the binding contract, not SQL grammar or schema correctness.
[[nodiscard]] constexpr DbSqlParameterCount countDbSqlParameters(std::string_view sql, DbDriver driver) noexcept {
    DbSqlParameterCount result;
    if (driver != DbDriver::kMariaDb && driver != DbDriver::kPostgreSql) {
        return {0, false};
    }
    for (std::size_t index = 0; index < sql.size();) {
        if (sql[index] == '\0') {
            return {0, false};
        }
        const auto next = driver == DbDriver::kMariaDb ? skipSqlAtom(sql, index) : skipPostgreSqlSqlAtom(sql, index);
        if (next != index + 1) {
            index = next;
            continue;
        }
        if (driver == DbDriver::kMariaDb) {
            result.count += sql[index] == '?';
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
            if (number > result.count) {
                result.count = number;
            }
            continue;
        }
        // Unquoted identifiers can contain '$'; account$1 is not a parameter.
        if (isPostgreSqlDollarTagStart(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80) {
            do {
                ++index;
            } while (index < sql.size() && (isPostgreSqlIdentifierContinue(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80));
        } else {
            ++index;
        }
    }
    return result;
}

template <FixedString Sql, DbDriver Driver, std::size_t ParameterCount>
void validateDbSqlLiteral(DbDriver actualDriver) {
    static_assert(Driver == DbDriver::kMariaDb || Driver == DbDriver::kPostgreSql,
        "SQL literal requires a concrete database driver");
    static_assert(Sql.view().find('\0') == std::string_view::npos, "SQL literal must not contain NUL bytes");
    constexpr auto result = countDbSqlParameters(Sql.view(), Driver);
    static_assert(result.valid, "SQL literal contains an invalid parameter index");
    static_assert(result.count == ParameterCount, "SQL placeholder count does not match the number of arguments");
    if (actualDriver != Driver) {
        throw std::invalid_argument("SQL literal dialect does not match the database driver");
    }
}

}  // namespace ruvia::detail
