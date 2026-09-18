#include <string_view>

#include "ruvia/web/detail/db/DbSqlLiteral.h"

#include "test_harness.h"

using ruvia::DbDriver;
using ruvia::detail::countDbSqlParameters;

RUVIA_TEST(sql_literal_mariadb_counts_only_parameter_tokens) {
    constexpr auto result = countDbSqlParameters(
        "SELECT '?', \"?\", `?`, 'it''s ?' FROM t WHERE a = ? AND b = ? /* ? */ -- ?\n# ?\n", DbDriver::kMariaDb);
    RUVIA_CHECK(result.valid);
    RUVIA_CHECK_EQ(result.count, std::size_t{2});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT --?", DbDriver::kMariaDb).count, std::size_t{1});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT 'a\\'?', ?", DbDriver::kMariaDb).count, std::size_t{1});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT 1", DbDriver::kMariaDb).count, std::size_t{0});
}

RUVIA_TEST(sql_literal_postgresql_counts_parameter_indices) {
    constexpr auto result = countDbSqlParameters(
        "SELECT $2, $1, $2, '$90', \"$80\", $$ $70 $$, $tag$ $60 $tag$, E'a\\' $50', account$40 "
        "/* outer /* $30 */ $20 */ --$10\n FROM t WHERE payload ? 'key'",
        DbDriver::kPostgreSql);
    RUVIA_CHECK(result.valid);
    RUVIA_CHECK_EQ(result.count, std::size_t{2});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT $1, $1", DbDriver::kPostgreSql).count, std::size_t{1});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT $标签$ $90 $标签$, $1", DbDriver::kPostgreSql).count, std::size_t{1});
    RUVIA_CHECK_EQ(countDbSqlParameters("SELECT 1", DbDriver::kPostgreSql).count, std::size_t{0});
    RUVIA_CHECK(!countDbSqlParameters("SELECT $0", DbDriver::kPostgreSql).valid);
    RUVIA_CHECK(!countDbSqlParameters("SELECT $999999999999999999999999999999999", DbDriver::kPostgreSql).valid);
    RUVIA_CHECK(!countDbSqlParameters("SELECT 1", DbDriver::kUnspecified).valid);
}

RUVIA_TEST(sql_literal_rejects_a_different_runtime_dialect) {
    ruvia::detail::validateDbSqlLiteral<"SELECT ?", DbDriver::kMariaDb, 1>(DbDriver::kMariaDb);
    ruvia::detail::validateDbSqlLiteral<"SELECT $1, $1", DbDriver::kPostgreSql, 1>(DbDriver::kPostgreSql);
    RUVIA_CHECK(ruvia::testing::throwsOn([] {
        ruvia::detail::validateDbSqlLiteral<"SELECT ?", DbDriver::kMariaDb, 1>(DbDriver::kPostgreSql);
    }));
}
