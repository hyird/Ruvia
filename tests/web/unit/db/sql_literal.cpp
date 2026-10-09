#include <string_view>

#include "ruvia/web/detail/db/db_sql_literal.h"

#include "test_harness.h"

using ruvia::db_driver;
using ruvia::detail::count_db_sql_parameters;

RUVIA_TEST(sql_literal_mariadb_counts_only_parameter_tokens) {
    constexpr auto result_value = count_db_sql_parameters(
        "SELECT '?', \"?\", `?`, 'it''s ?' FROM t WHERE a = ? AND b = ? /* ? */ -- ?\n# ?\n", db_driver::mariadb);
    RUVIA_CHECK(result_value.valid_);
    RUVIA_CHECK_EQ(result_value.count_, std::size_t{2});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT --?", db_driver::mariadb).count_, std::size_t{1});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT 'a\\'?', ?", db_driver::mariadb).count_, std::size_t{1});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT 1", db_driver::mariadb).count_, std::size_t{0});
}

RUVIA_TEST(sql_literal_postgresql_counts_parameter_indices) {
    constexpr auto result_value = count_db_sql_parameters(
        "SELECT $2, $1, $2, '$90', \"$80\", $$ $70 $$, $tag$ $60 $tag$, E'a\\' $50', account$40 "
        "/* outer /* $30 */ $20 */ --$10\n FROM t WHERE payload ? 'key'",
        db_driver::postgresql);
    RUVIA_CHECK(result_value.valid_);
    RUVIA_CHECK_EQ(result_value.count_, std::size_t{2});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT $1, $1", db_driver::postgresql).count_, std::size_t{1});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT $标签$ $90 $标签$, $1", db_driver::postgresql).count_, std::size_t{1});
    RUVIA_CHECK_EQ(count_db_sql_parameters("SELECT 1", db_driver::postgresql).count_, std::size_t{0});
    RUVIA_CHECK(!count_db_sql_parameters("SELECT $0", db_driver::postgresql).valid_);
    RUVIA_CHECK(!count_db_sql_parameters("SELECT $999999999999999999999999999999999", db_driver::postgresql).valid_);
    RUVIA_CHECK(!count_db_sql_parameters("SELECT 1", db_driver::unspecified).valid_);
}

RUVIA_TEST(sql_literal_rejects_a_different_runtime_dialect) {
    ruvia::detail::validate_db_sql_literal<"SELECT ?", db_driver::mariadb, 1>(db_driver::mariadb);
    ruvia::detail::validate_db_sql_literal<"SELECT $1, $1", db_driver::postgresql, 1>(db_driver::postgresql);
    RUVIA_CHECK(ruvia::testing::throws_on([] {
        ruvia::detail::validate_db_sql_literal<"SELECT ?", db_driver::mariadb, 1>(db_driver::postgresql);
    }));
}
