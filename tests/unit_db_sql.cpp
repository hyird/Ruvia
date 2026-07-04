#include "test_harness.h"

#include <mysql/mysql.h>

#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "db/core/DbSql.h"
#include "ruvia/db/DbTypes.h"

namespace {

using ruvia::DbValue;
using ruvia::detail::interpolateSql;

std::string interp(st_mysql& conn, std::string_view sql, const std::vector<DbValue>& params) {
    auto out = interpolateSql(
        conn, sql, std::span<const DbValue>(params.data(), params.size()),
        std::pmr::get_default_resource());
    return std::string(out.data(), out.size());
}

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(db_interpolate_sql_quotes_and_escapes_strings) {
    // mysql_init yields a client handle that escapes with the default charset
    // without needing a server connection.
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A string param is single-quoted and its embedded quote is escaped, so it
    // cannot terminate the literal early (the SQL-injection defense).
    const auto out = interp(mysql, "WHERE name = ?", {DbValue(std::string_view("a'b"))});
    RUVIA_CHECK_EQ(out, std::string("WHERE name = 'a\\'b'"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_renders_typed_literals) {
    MYSQL mysql;
    mysql_init(&mysql);

    // Numbers and booleans are emitted as typed literals (never string-quoted),
    // null becomes the SQL keyword, and strings are quoted.
    RUVIA_CHECK_EQ(
        interp(mysql, "VALUES (?, ?, ?, ?)",
               {DbValue(42), DbValue(true), DbValue(nullptr), DbValue(std::string_view("x"))}),
        std::string("VALUES (42, 1, NULL, 'x')"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_requires_matching_placeholder_count) {
    MYSQL mysql;
    mysql_init(&mysql);

    // Fewer params than placeholders, and more params than placeholders, both
    // throw rather than silently producing malformed SQL.
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "? ?", {DbValue(1)}); }));
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "?", {DbValue(1), DbValue(2)}); }));
    // No placeholders and no params passes through unchanged.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1", {}), std::string("SELECT 1"));

    mysql_close(&mysql);
}
