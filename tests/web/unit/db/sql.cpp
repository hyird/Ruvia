#include "test_harness.h"

#include <mysql/mysql.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbMigrationChecksum.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbSql.h"
#include "ruvia/web/detail/db/DbSqlScan.h"
#include "ruvia/web/db/DbTypes.h"

namespace {

using ruvia::DbValue;
using ruvia::detail::interpolateSql;

std::string interp(st_mysql& conn, std::string_view sql, const std::vector<DbValue>& params) {
    auto out = interpolateSql(conn, sql, std::span<const DbValue>(params.data(), params.size()), std::pmr::get_default_resource());
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
    RUVIA_CHECK_EQ(interp(mysql, "VALUES (?, ?, ?, ?)", {DbValue(42), DbValue(true), DbValue(nullptr), DbValue(std::string_view("x"))}), std::string("VALUES (42, 1, NULL, 'x')"));

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
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "VALUES (?)", {DbValue(std::numeric_limits<double>::infinity())}); }));
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "VALUES (?)", {DbValue(-std::numeric_limits<double>::infinity())}); }));
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "VALUES (?)", {DbValue(std::numeric_limits<double>::quiet_NaN())}); }));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_escapes_backslash_and_injection_payloads) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A backslash is itself a MySQL escape character, so it must be doubled --
    // otherwise a value ending in '\' would consume the closing quote and break out
    // of the literal. This is a distinct escape path from the single-quote case and
    // is the classic backslash-breakout bypass.
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("a\\b"))}), std::string("WHERE p = 'a\\\\b'"));
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("x\\"))}), std::string("WHERE p = 'x\\\\'"));

    // A full injection payload: every quote is escaped so it can neither terminate
    // the literal nor append a clause.
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("' OR '1'='1"))}), std::string("WHERE p = '\\' OR \\'1\\'=\\'1'"));

    // A '?' inside a PARAMETER VALUE is data, not a placeholder: it is escaped as an
    // ordinary character and never consumes a parameter slot (the placeholder scan
    // walks the SQL template, not the substituted values).
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {DbValue(std::string_view("a?b"))}), std::string("WHERE p = 'a?b'"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_binds_only_statement_level_placeholders) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A '?' inside a string literal is part of the value, not a placeholder:
    // the statement keeps it and the one real placeholder takes the parameter.
    RUVIA_CHECK_EQ(interp(mysql, "UPDATE t SET note = 'a?b' WHERE id = ?", {DbValue(std::int64_t{7})}),
        std::string("UPDATE t SET note = 'a?b' WHERE id = 7"));
    // Binding the literal's '?' used to consume the first parameter and shift
    // every later one along, producing SQL that still ran and wrote the wrong
    // rows ("note = 'a7b' WHERE id = 'X'"). That statement has one placeholder,
    // so a second parameter is now a reported mismatch instead.
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "UPDATE t SET note = 'a?b' WHERE id = ?", {DbValue(std::int64_t{7}), DbValue(std::string_view("X"))}); }));
    RUVIA_CHECK_EQ(interp(mysql, "UPDATE t SET note = 'why?' WHERE id = ?", {DbValue(std::int64_t{7})}),
        std::string("UPDATE t SET note = 'why?' WHERE id = 7"));

    // The same holds for every construct that can carry an opaque byte: quoted
    // identifiers, both line-comment forms, and block comments.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT `we?rd` FROM t WHERE id = ?", {DbValue(std::int64_t{1})}),
        std::string("SELECT `we?rd` FROM t WHERE id = 1"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1 -- really?\n WHERE id = ?", {DbValue(std::int64_t{2})}),
        std::string("SELECT 1 -- really?\n WHERE id = 2"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1 # really?\n WHERE id = ?", {DbValue(std::int64_t{3})}),
        std::string("SELECT 1 # really?\n WHERE id = 3"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT /* ? */ 1 WHERE id = ?", {DbValue(std::int64_t{4})}),
        std::string("SELECT /* ? */ 1 WHERE id = 4"));

    // A doubled quote escapes the quote rather than closing the literal, so the
    // scan must not resume inside what is still one string.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 'a''?''b' WHERE id = ?", {DbValue(std::int64_t{5})}),
        std::string("SELECT 'a''?''b' WHERE id = 5"));
    // An escaped quote does not close it either.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 'a\\'?' WHERE id = ?", {DbValue(std::int64_t{6})}),
        std::string("SELECT 'a\\'?' WHERE id = 6"));

    // A placeholder immediately after a skipped construct is still bound.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT '?'?", {DbValue(std::int64_t{8})}), std::string("SELECT '?'8"));

    // Placeholders that only exist inside literals are not placeholders, so a
    // parameter for them is an error rather than a silent substitution.
    RUVIA_CHECK(throwsOn([&] { (void)interp(mysql, "SELECT 'only?'", {DbValue(1)}); }));

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
    constexpr auto driver = ruvia::DbDriver::kMariaDb;
    RUVIA_CHECK(isValidMigrationTableName("ruvia_schema_migrations", driver));
    RUVIA_CHECK(isValidMigrationTableName("t1", driver));
    RUVIA_CHECK(isValidMigrationTableName("_private", driver));
    RUVIA_CHECK(isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    // The name is a non-parameterizable identifier, so anything that could break
    // out of the backtick quoting or restructure the SQL is rejected.
    RUVIA_CHECK(!isValidMigrationTableName("", driver));
    RUVIA_CHECK(!isValidMigrationTableName("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    RUVIA_CHECK(!isValidMigrationTableName("has space", driver));
    RUVIA_CHECK(!isValidMigrationTableName("has-hyphen", driver));
    RUVIA_CHECK(!isValidMigrationTableName("a.b", driver));
    RUVIA_CHECK(!isValidMigrationTableName("quote'", driver));
    RUVIA_CHECK(!isValidMigrationTableName("tbl`; DROP TABLE users;--", driver));
}

RUVIA_TEST(db_migration_list_validation_enforces_integrity) {
    using ruvia::DbMigration;
    using ruvia::detail::validateMigrationList;
    const DbMigration ok[] = {{"001_init", "CREATE TABLE a(id INT)"}, {"002_more", "ALTER TABLE a ADD b INT"}};
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

    // Ids that differ only in letter case are one id to a case-insensitive
    // collation: MariaDB's default would report the second as already applied
    // and never run it, while PostgreSQL would apply both. Refused here so one
    // list cannot produce two schemas.
    const DbMigration caseDup[] = {{"v1_users", "SQL1"}, {"V1_Users", "SQL2"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(caseDup, 2)); }));
    // Ids that differ in more than case remain distinct.
    const DbMigration distinct[] = {{"v1_users", "SQL1"}, {"v2_users", "SQL2"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(distinct, 2)); }));

    // MariaDB collations are PAD SPACE -- the binary one the table pins
    // included -- so "v1" and "v1 " would be one row there and two on
    // PostgreSQL. A surrounded id is refused rather than folded.
    const DbMigration padded[] = {{"v1_users ", "SQL1"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(padded, 1)); }));
    const DbMigration leading[] = {{" v1_users", "SQL1"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(leading, 1)); }));
    // Interior spaces are not the ambiguity; they compare exactly.
    const DbMigration interior[] = {{"v1 users", "SQL1"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(interior, 1)); }));
}

RUVIA_TEST(db_migration_list_validation_enforces_one_statement) {
    using ruvia::DbMigration;
    using ruvia::detail::validateMigrationList;

    // Neither backend runs two statements in one call, so the packaging error
    // is reported here instead of arriving as a backend syntax error pointing
    // at the second statement.
    const DbMigration two[] = {{"001", "CREATE TABLE a(id INT); CREATE TABLE b(id INT)"}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(two, 1)); }));

    // A trailing separator is accepted by both backends, so it is accepted
    // here -- with or without trailing whitespace.
    const DbMigration trailing[] = {{"001", "CREATE TABLE a(id INT);"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailing, 1)); }));
    const DbMigration trailingSpace[] = {{"001", "CREATE TABLE a(id INT);\n  "}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailingSpace, 1)); }));

    // A ';' that is data -- inside a default value, a quoted identifier or a
    // comment -- is not a statement separator.
    const DbMigration quoted[] = {{"001", "CREATE TABLE a(id INT, s VARCHAR(4) DEFAULT 'a;b')"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(quoted, 1)); }));
    const DbMigration commented[] = {{"001", "CREATE TABLE a(id INT) -- one; two\n"}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(commented, 1)); }));
}

RUVIA_TEST(db_migration_checksum_pins_the_recorded_text) {
    using ruvia::detail::kMigrationChecksumSize;
    using ruvia::detail::migrationChecksum;

    // The published SHA-256 vector for "abc", lowercase hex: a stored checksum
    // has to keep meaning the same thing across releases, so the digest and its
    // encoding are pinned rather than merely self-consistent.
    const auto abc = migrationChecksum("abc", std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string(abc.data(), abc.size()), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    RUVIA_CHECK_EQ(abc.size(), kMigrationChecksumSize);

    // Same text, same digest; one edited byte, a different one.
    const auto first = migrationChecksum("CREATE TABLE a(id INT)", std::pmr::get_default_resource());
    const auto again = migrationChecksum("CREATE TABLE a(id INT)", std::pmr::get_default_resource());
    const auto edited = migrationChecksum("CREATE TABLE a(id BIGINT)", std::pmr::get_default_resource());
    RUVIA_CHECK(first == again);
    RUVIA_CHECK(first != edited);
}

RUVIA_TEST(db_migration_carries_its_atomicity) {
    using ruvia::DbMigration;
    using ruvia::DbMigrationAtomicity;

    // Committing the statement and the row that records it together is the
    // default; naming the exception is opt-in and per migration, so one
    // statement that cannot run in a transaction block does not cost the rest
    // of the list its atomicity.
    constexpr DbMigration standard{"001", "CREATE TABLE a(id INT)"};
    static_assert(standard.atomicity() == DbMigrationAtomicity::kTransactional);
    constexpr DbMigration concurrent{"002", "CREATE INDEX CONCURRENTLY i ON a (id)", DbMigrationAtomicity::kUnwrapped};
    static_assert(concurrent.atomicity() == DbMigrationAtomicity::kUnwrapped);
    RUVIA_CHECK(standard.atomicity() == DbMigrationAtomicity::kTransactional);
    RUVIA_CHECK(concurrent.atomicity() == DbMigrationAtomicity::kUnwrapped);
}

RUVIA_TEST(db_sql_scan_steps_over_opaque_constructs) {
    using ruvia::detail::findSqlSyntaxByte;
    using ruvia::detail::skipSqlAtom;

    // The scan is shared by the parameter binder and the migration validator,
    // so its own boundaries are pinned here.
    RUVIA_CHECK_EQ(skipSqlAtom("abc", 0), std::size_t{1});
    RUVIA_CHECK_EQ(skipSqlAtom("'ab'x", 0), std::size_t{4});
    RUVIA_CHECK_EQ(skipSqlAtom("'a''b'x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skipSqlAtom("`a``b`x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skipSqlAtom("-- c\nx", 0), std::size_t{5});
    RUVIA_CHECK_EQ(skipSqlAtom("/* c */x", 0), std::size_t{7});
    // A backslash escapes inside quotes but never inside a quoted identifier,
    // where MariaDB treats it as an ordinary byte.
    RUVIA_CHECK_EQ(skipSqlAtom("'a\\'b'x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skipSqlAtom("`a\\`x", 0), std::size_t{4});
    // An unterminated construct consumes the rest rather than running past the
    // end; the caller then reports a mismatch instead of binding into it.
    RUVIA_CHECK_EQ(skipSqlAtom("'abc", 0), std::size_t{4});
    RUVIA_CHECK_EQ(skipSqlAtom("/* abc", 0), std::size_t{6});
    // A lone '-' or '/' is an operator, not a comment.
    RUVIA_CHECK_EQ(skipSqlAtom("a-b", 1), std::size_t{2});
    RUVIA_CHECK_EQ(skipSqlAtom("a/b", 1), std::size_t{2});

    RUVIA_CHECK_EQ(findSqlSyntaxByte("a;b", ';'), std::size_t{1});
    RUVIA_CHECK_EQ(findSqlSyntaxByte("'a;b'", ';'), std::string_view::npos);
    RUVIA_CHECK_EQ(findSqlSyntaxByte("'a;b';", ';'), std::size_t{5});
    RUVIA_CHECK_EQ(findSqlSyntaxByte("x", '?'), std::string_view::npos);
}

RUVIA_TEST(db_config_validation_checks_every_field) {
    using ruvia::DbConfig;
    using ruvia::detail::validateDbConfig;
    using std::chrono::milliseconds;

    static_assert(std::same_as<decltype(DbConfig{}.connectTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(DbConfig{}.readTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(DbConfig{}.writeTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(DbConfig{}.queryTimeout), std::optional<milliseconds>>);
    static_assert(std::same_as<decltype(DbConfig{}.acquireTimeout), std::optional<milliseconds>>);

    // A default config is valid; absent timeouts are disabled explicitly.
    RUVIA_CHECK(!throwsOn([] { validateDbConfig(DbConfig{}); }));

    // Host and port each have a required-value guard.
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.host.clear();
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.port = 0;
        validateDbConfig(c);
    }));

    // Every configured timeout must be positive. Zero cannot silently recover the
    // former sentinel convention, and the whole fold must validate every field.
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.connectTimeout = milliseconds(0);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.readTimeout = milliseconds(0);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.writeTimeout = milliseconds(0);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.queryTimeout = milliseconds(0);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.acquireTimeout = milliseconds(0);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.connectTimeout = milliseconds(-1);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.readTimeout = milliseconds(-1);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.writeTimeout = milliseconds(-1);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.queryTimeout = milliseconds(-1);
        validateDbConfig(c);
    }));
    RUVIA_CHECK(throwsOn([] {
        DbConfig c;
        c.acquireTimeout = milliseconds(-1);
        validateDbConfig(c);
    }));
}
