#include <mysql.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_sql_scan.h"

#include "db/db_config_validation.h"
#include "db/db_migration_checksum.h"
#include "db/db_migration_validation.h"
#include "db/db_sql.h"
#include "test_harness.h"

namespace {

using ruvia::db_value;
using ruvia::detail::interpolate_sql;
using ruvia::testing::throws_on;

std::string interp(st_mysql& conn, std::string_view sql, const std::vector<db_value>& params) {
    auto out = interpolate_sql(conn, sql, std::span<const db_value>(params.data(), params.size()),
        std::pmr::get_default_resource());
    return std::string(out.data(), out.size());
}

template <typename fn_type>
bool throws_length(fn_type&& fn) {
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
    const auto out = interp(mysql, "WHERE name = ?", {db_value(std::string_view("a'b"))});
    RUVIA_CHECK_EQ(out, std::string("WHERE name = 'a\\'b'"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_preserves_binary_strings_and_full_escape_expansion) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    RUVIA_CHECK_EQ(interp(mysql, "SELECT ?, ?, ?", {db_value(std::string_view{}), db_value(std::string_view("\0\n\r\\'\"\x1a", 7)), db_value(42)}),
        std::string("SELECT '', '\\0\\n\\r\\\\\\'\\\"\\Z', 42"));

    for (const std::size_t size : {15, 16, 4096, 65536}) {
        const std::string value(size, '\'');
        std::string expected = "SELECT '";
        for (std::size_t index = 0; index < size; ++index) {
            expected.append("\\'");
        }
        expected.append("', 'tail'");
        RUVIA_CHECK_EQ(interp(mysql, "SELECT ?, ?", {db_value(std::string_view(value)), db_value(std::string_view("tail"))}), expected);
    }

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_renders_typed_literals) {
    MYSQL mysql;
    mysql_init(&mysql);

    // Numbers and booleans are emitted as typed literals (never string-quoted),
    // null becomes the SQL keyword, and strings are quoted.
    RUVIA_CHECK_EQ(
        interp(mysql, "VALUES (?, ?, ?, ?)",
            {db_value(42), db_value(true), db_value(nullptr), db_value(std::string_view("x"))}),
        std::string("VALUES (42, 1, NULL, 'x')"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_double_finite_renders_nonfinite_rejected) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A finite double renders as an unquoted numeric literal (the typed-literals
    // test covered int/bool/null/string but never a double).
    RUVIA_CHECK_EQ(interp(mysql, "VALUES (?)", {db_value(3.5)}), std::string("VALUES (3.5)"));

    // A non-finite double must be REJECTED, not spliced as the bare words "inf"/"nan"
    // that std::to_chars produces -- those are not valid SQL numerics and would land
    // UNQUOTED in the statement. Positive/negative infinity and NaN all throw.
    RUVIA_CHECK(throws_on([&] {
        (void)interp(mysql, "VALUES (?)", {db_value(std::numeric_limits<double>::infinity())});
    }));
    RUVIA_CHECK(throws_on([&] {
        (void)interp(mysql, "VALUES (?)", {db_value(-std::numeric_limits<double>::infinity())});
    }));
    RUVIA_CHECK(throws_on([&] {
        (void)interp(mysql, "VALUES (?)", {db_value(std::numeric_limits<double>::quiet_NaN())});
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
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {db_value(std::string_view("a\\b"))}),
        std::string("WHERE p = 'a\\\\b'"));
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {db_value(std::string_view("x\\"))}),
        std::string("WHERE p = 'x\\\\'"));

    // A full injection payload: every quote is escaped so it can neither terminate
    // the literal nor append a clause.
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {db_value(std::string_view("' OR '1'='1"))}),
        std::string("WHERE p = '\\' OR \\'1\\'=\\'1'"));

    // A '?' inside a PARAMETER VALUE is data, not a placeholder: it is escaped as an
    // ordinary character and never consumes a parameter slot (the placeholder scan
    // walks the SQL template, not the substituted values).
    RUVIA_CHECK_EQ(interp(mysql, "WHERE p = ?", {db_value(std::string_view("a?b"))}),
        std::string("WHERE p = 'a?b'"));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_binds_only_statement_level_placeholders) {
    MYSQL mysql;
    RUVIA_CHECK(mysql_init(&mysql) != nullptr);

    // A '?' inside a string literal is part of the value, not a placeholder:
    // the statement keeps it and the one real placeholder takes the parameter.
    RUVIA_CHECK_EQ(
        interp(mysql, "UPDATE t SET note = 'a?b' WHERE id = ?", {db_value(std::int64_t{7})}),
        std::string("UPDATE t SET note = 'a?b' WHERE id = 7"));
    // Binding the literal's '?' used to consume the first parameter and shift
    // every later one along, producing SQL that still ran and wrote the wrong
    // rows ("note = 'a7b' WHERE id = 'X'"). That statement has one placeholder,
    // so a second parameter is now a reported mismatch instead.
    RUVIA_CHECK(throws_on([&] {
        (void)interp(mysql, "UPDATE t SET note = 'a?b' WHERE id = ?",
            {db_value(std::int64_t{7}), db_value(std::string_view("X"))});
    }));
    RUVIA_CHECK_EQ(
        interp(mysql, "UPDATE t SET note = 'why?' WHERE id = ?", {db_value(std::int64_t{7})}),
        std::string("UPDATE t SET note = 'why?' WHERE id = 7"));

    // The same holds for every construct that can carry an opaque byte: quoted
    // identifiers, both line-comment forms, and block comments.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT `we?rd` FROM t WHERE id = ?", {db_value(std::int64_t{1})}),
        std::string("SELECT `we?rd` FROM t WHERE id = 1"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1 -- really?\n WHERE id = ?", {db_value(std::int64_t{2})}),
        std::string("SELECT 1 -- really?\n WHERE id = 2"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 1 # really?\n WHERE id = ?", {db_value(std::int64_t{3})}),
        std::string("SELECT 1 # really?\n WHERE id = 3"));
    RUVIA_CHECK_EQ(interp(mysql, "SELECT /* ? */ 1 WHERE id = ?", {db_value(std::int64_t{4})}),
        std::string("SELECT /* ? */ 1 WHERE id = 4"));
    // MySQL/MariaDB require "--" comments to be followed by whitespace or a
    // control byte. Without that byte, the following "?" remains a placeholder.
    RUVIA_CHECK_EQ(
        interp(mysql, "SELECT 1--?", {db_value(std::int64_t{9})}), std::string("SELECT 1--9"));

    // A doubled quote escapes the quote rather than closing the literal, so the
    // scan must not resume inside what is still one string.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 'a''?''b' WHERE id = ?", {db_value(std::int64_t{5})}),
        std::string("SELECT 'a''?''b' WHERE id = 5"));
    // An escaped quote does not close it either.
    RUVIA_CHECK_EQ(interp(mysql, "SELECT 'a\\'?' WHERE id = ?", {db_value(std::int64_t{6})}),
        std::string("SELECT 'a\\'?' WHERE id = 6"));

    // A placeholder immediately after a skipped construct is still bound.
    RUVIA_CHECK_EQ(
        interp(mysql, "SELECT '?'?", {db_value(std::int64_t{8})}), std::string("SELECT '?'8"));

    // Placeholders that only exist inside literals are not placeholders, so a
    // parameter for them is an error rather than a silent substitution.
    RUVIA_CHECK(throws_on([&] { (void)interp(mysql, "SELECT 'only?'", {db_value(1)}); }));

    mysql_close(&mysql);
}

RUVIA_TEST(db_interpolate_sql_requires_matching_placeholder_count) {
    MYSQL mysql;
    mysql_init(&mysql);

    // Fewer params than placeholders, and more params than placeholders, both
    // throw rather than silently producing malformed SQL.
    RUVIA_CHECK(throws_on([&] { (void)interp(mysql, "? ?", {db_value(1)}); }));
    RUVIA_CHECK(throws_on([&] { (void)interp(mysql, "?", {db_value(1), db_value(2)}); }));
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
    constexpr auto too_large_to_escape = (std::numeric_limits<unsigned long>::max)() / 2 + 1;
    if constexpr (too_large_to_escape <= (std::numeric_limits<std::size_t>::max)()) {
        const auto oversized =
            std::string_view(&sentinel, static_cast<std::size_t>(too_large_to_escape));
        RUVIA_CHECK(throws_length([&] { (void)interp(mysql, "VALUES (?)", {db_value(oversized)}); }));
    }

    if constexpr ((std::numeric_limits<std::size_t>::max)() >
                  (std::numeric_limits<unsigned long>::max)()) {
        RUVIA_CHECK(throws_length([&] {
            ruvia::detail::validate_mariadb_sql_length(
                static_cast<std::size_t>((std::numeric_limits<unsigned long>::max)()) + 1);
        }));
    }

    mysql_close(&mysql);
}

RUVIA_TEST(db_config_validation_checks_every_field) {
    using ruvia::db_config;
    using ruvia::db_driver;
    using ruvia::detail::validate_db_config;
    using std::chrono::milliseconds;

    RUVIA_CHECK(throws_on([] { validate_db_config(db_config{}); }));

    // An omitted port selects the driver's standard port during normalization.
    const auto defaults = db_config{.driver_ = db_driver::mariadb};
    RUVIA_CHECK(!defaults.port_.has_value());
    RUVIA_CHECK_EQ(ruvia::detail::configured_db_port(defaults), std::uint16_t{3306});
    RUVIA_CHECK(defaults.connect_timeout_ == std::chrono::seconds(5));
    RUVIA_CHECK(defaults.query_timeout_ == std::chrono::seconds(30));
    RUVIA_CHECK(defaults.acquire_timeout_ == std::chrono::seconds(5));
    RUVIA_CHECK(!defaults.read_timeout_.has_value());
    RUVIA_CHECK(!defaults.write_timeout_.has_value());
    RUVIA_CHECK(!throws_on([] { validate_db_config(db_config{.driver_ = db_driver::mariadb}); }));

    // Explicit absence is the only way to request an unbounded operation.
    RUVIA_CHECK(!throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.connect_timeout_ = std::nullopt;
        c.query_timeout_ = std::nullopt;
        c.acquire_timeout_ = std::nullopt;
        validate_db_config(c);
    }));

    // Host and port each have a required-value guard.
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.host_.clear();
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.port_ = 0;
        validate_db_config(c);
    }));

    // Every configured timeout must be positive. Zero cannot silently recover the
    // former sentinel convention, and the whole fold must validate every field.
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.connect_timeout_ = milliseconds(0);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.read_timeout_ = milliseconds(0);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.write_timeout_ = milliseconds(0);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.query_timeout_ = milliseconds(0);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.acquire_timeout_ = milliseconds(0);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.connect_timeout_ = milliseconds(-1);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.read_timeout_ = milliseconds(-1);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.write_timeout_ = milliseconds(-1);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.query_timeout_ = milliseconds(-1);
        validate_db_config(c);
    }));
    RUVIA_CHECK(throws_on([] {
        auto c = db_config{.driver_ = db_driver::mariadb};
        c.acquire_timeout_ = milliseconds(-1);
        validate_db_config(c);
    }));
}
