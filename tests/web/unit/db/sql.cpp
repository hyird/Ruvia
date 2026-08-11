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
#include <stdexcept>
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

template <typename Fn>
bool throwsLength(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::length_error&) {
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
    // MySQL/MariaDB require "--" comments to be followed by whitespace or a
    // control byte. Without that byte, the following "?" remains a placeholder.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1--?", {DbValue(std::int64_t{9})}),
        std::string("SELECT 1--9"));

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

RUVIA_TEST(db_interpolate_sql_rejects_unrepresentable_lengths) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // The input is intentionally a non-dereferenced oversized view. The
    // length guard must run before the escape library sees it or the size hint
    // performs value.size() * 2.
    const char sentinel = 'x';
    constexpr auto tooLargeToEscape = (std::numeric_limits<unsigned long>::max)() / 2 + 1;
    if constexpr (tooLargeToEscape <= (std::numeric_limits<std::size_t>::max)()) {
        const auto oversized = std::string_view(&sentinel, static_cast<std::size_t>(tooLargeToEscape));
        RUVIA_CHECK(throwsLength([&] { (void)interp(mysql, "VALUES (?)", {DbValue(oversized)}); }));
    }

    if constexpr ((std::numeric_limits<std::size_t>::max)() > (std::numeric_limits<unsigned long>::max)()) {
        RUVIA_CHECK(throwsLength([&] {
            ruvia::detail::validateMariaDbSqlLength(static_cast<std::size_t>((std::numeric_limits<unsigned long>::max)()) + 1);
        }));
    }

    mysql_close(&mysql);
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
