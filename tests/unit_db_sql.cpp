#include "test_harness.h"

#include <mysql/mysql.h>

#include <exception>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbSql.h"
#include "ruvia/web/db/DbTypes.h"

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

RUVIA_TEST(db_interpolate_sql_double_finite_renders_nonfinite_rejected) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A finite double renders as an unquoted numeric literal (the typed-literals
    // test covered int/bool/null/string but never a double).
    RUVIA_CHECK_EQ(interp(mysql, "VALUES (?)", {DbValue(3.5)}), std::string("VALUES (3.5)"));

    // A non-finite double must be REJECTED, not spliced as the bare words "inf"/"nan"
    // that std::to_chars produces -- those are not valid SQL numerics and would land
    // UNQUOTED in the statement. Positive/negative infinity and NaN all throw.
    RUVIA_CHECK(throwsOn([&] {
        (void)interp(mysql, "VALUES (?)", {DbValue(std::numeric_limits<double>::infinity())});
    }));
    RUVIA_CHECK(throwsOn([&] {
        (void)interp(mysql, "VALUES (?)", {DbValue(-std::numeric_limits<double>::infinity())});
    }));
    RUVIA_CHECK(throwsOn([&] {
        (void)interp(mysql, "VALUES (?)", {DbValue(std::numeric_limits<double>::quiet_NaN())});
    }));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_escapes_backslash_and_injection_payloads) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A backslash is itself a MySQL escape character, so it must be doubled --
    // otherwise a value ending in '\' would consume the closing quote and break out
    // of the literal. This is a distinct escape path from the single-quote case and
    // is the classic backslash-breakout bypass.
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("a\\b"))}),
                   std::string("WHERE p = 'a\\\\b'"));
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("x\\"))}),
                   std::string("WHERE p = 'x\\\\'"));

    // A full injection payload: every quote is escaped so it can neither terminate
    // the literal nor append a clause.
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("' OR '1'='1"))}),
                   std::string("WHERE p = '\\' OR \\'1\\'=\\'1'"));

    // A '?' inside a PARAMETER VALUE is data, not a placeholder: it is escaped as an
    // ordinary character and never consumes a parameter slot (the placeholder scan
    // walks the SQL template, not the substituted values).
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("a?b"))}),
                   std::string("WHERE p = 'a?b'"));

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

RUVIA_TEST(db_migration_table_name_rejects_injection) {
    using ruvia::detail::isValidMigrationTableName;
    // Valid SQL identifiers: letters, digits, underscores.
    RUVIA_CHECK(isValidMigrationTableName("ruvia_schema_migrations"));
    RUVIA_CHECK(isValidMigrationTableName("t1"));
    RUVIA_CHECK(isValidMigrationTableName("_private"));
    RUVIA_CHECK(isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    // The name is a non-parameterizable identifier, so anything that could break
    // out of the backtick quoting or restructure the SQL is rejected.
    RUVIA_CHECK(!isValidMigrationTableName(""));
    RUVIA_CHECK(!isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    RUVIA_CHECK(!isValidMigrationTableName("has space"));
    RUVIA_CHECK(!isValidMigrationTableName("has-hyphen"));
    RUVIA_CHECK(!isValidMigrationTableName("a.b"));
    RUVIA_CHECK(!isValidMigrationTableName("quote'"));
    RUVIA_CHECK(!isValidMigrationTableName("tbl`; DROP TABLE users;--"));
}

RUVIA_TEST(db_migration_list_validation_enforces_integrity) {
    using ruvia::DbMigration;
    using ruvia::detail::validateMigrationList;
    const DbMigration ok[] = {
        {"001_init", "CREATE TABLE a(id INT)"},
        {"002_more", "ALTER TABLE a ADD b INT"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(ok, 2)); }));
    // An empty list is valid: nothing to apply.
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>()); }));
    // Duplicate ids would apply the wrong migration -> rejected.
    const DbMigration dup[] = {{"001", "SQL1"}, {"001", "SQL2"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(dup, 2)); }));
    // Empty id and empty SQL are rejected.
    const DbMigration emptyId[] = {{"", "SQL"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(emptyId, 1)); }));
    const DbMigration emptySql[] = {{"001", ""}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(emptySql, 1)); }));
    // An id longer than the 190-byte schema column is rejected.
    const std::string longId(191, 'x');
    const DbMigration tooLong[] = {{longId, "SQL"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(tooLong, 1)); }));
}

RUVIA_TEST(db_config_validation_checks_every_field) {
    using ruvia::DbConfig;
    using ruvia::detail::validateDbConfig;
    using std::chrono::milliseconds;

    // A default config is valid (localhost, port 3306, pool 4, timeouts 0).
    RUVIA_CHECK(!throwsOn([] { validateDbConfig(DbConfig{}); }));

    // Host, port and pool size each have a required-value guard.
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.host.clear(); validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.port = 0; validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.poolSize = 0; validateDbConfig(c); }));

    // Every one of the five timeouts must be non-negative -- a negative value in
    // any of them is rejected (verifies the whole fold is wired, not just one).
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.connectTimeout = milliseconds(-1); validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.readTimeout = milliseconds(-1); validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.writeTimeout = milliseconds(-1); validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.queryTimeout = milliseconds(-1); validateDbConfig(c); }));
    RUVIA_CHECK(throwsOn([] { DbConfig c; c.acquireTimeout = milliseconds(-1); validateDbConfig(c); }));
}
