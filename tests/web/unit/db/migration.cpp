// Migration bookkeeping is backend-neutral: id and SQL validation, the digest
// recorded with an applied migration, and the scan both drivers use to tell
// statement syntax from data. Nothing here needs a driver's client library, so
// it is compiled for either of them rather than only alongside MariaDB.

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_sql_scan.h"

#include "db/db_migration_checksum.h"
#include "db/db_migration_validation.h"
#include "test_harness.h"

using ruvia::testing::throws_on;

RUVIA_TEST(db_migrator_options_default_table_uses_ruvia_name) {
    const ruvia::db_migrator_options options;
    RUVIA_CHECK(options.table_ == "ruvia_schema_migrations");
}

RUVIA_TEST(db_migration_table_name_rejects_injection) {
    using ruvia::detail::is_valid_migration_table_name;
    // Valid SQL identifiers: letters, digits, underscores.
    constexpr auto driver = ruvia::db_driver::mariadb;
    RUVIA_CHECK(is_valid_migration_table_name("ruvia_schema_migrations", driver));
    RUVIA_CHECK(is_valid_migration_table_name("t1", driver));
    RUVIA_CHECK(is_valid_migration_table_name("_private", driver));
    RUVIA_CHECK(is_valid_migration_table_name(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    // The name is a non-parameterizable identifier, so anything that could break
    // out of the backtick quoting or restructure the SQL is rejected.
    RUVIA_CHECK(!is_valid_migration_table_name("", driver));
    RUVIA_CHECK(!is_valid_migration_table_name(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", driver));
    RUVIA_CHECK(!is_valid_migration_table_name("has space", driver));
    RUVIA_CHECK(!is_valid_migration_table_name("has-hyphen", driver));
    RUVIA_CHECK(!is_valid_migration_table_name("a.b", driver));
    RUVIA_CHECK(!is_valid_migration_table_name("quote'", driver));
    RUVIA_CHECK(!is_valid_migration_table_name("tbl`; DROP TABLE users;--", driver));
}

RUVIA_TEST(db_migration_postgresql_lock_timeout_conversion_is_checked) {
    using ruvia::detail::postgres_lock_timeout_milliseconds;

    const auto largest_representable_seconds =
        std::chrono::seconds(std::chrono::milliseconds::max().count() / 1000);
    RUVIA_CHECK_EQ(postgres_lock_timeout_milliseconds(largest_representable_seconds),
        static_cast<std::uint64_t>(largest_representable_seconds.count()) * 1000U);

    bool overflow_rejected = false;
    try {
        (void)postgres_lock_timeout_milliseconds(std::chrono::seconds::max());
    } catch (const std::invalid_argument&) {
        overflow_rejected = true;
    }
    RUVIA_CHECK(overflow_rejected);
}

RUVIA_TEST(db_migration_list_validation_enforces_integrity) {
    using ruvia::db_migration;
    using ruvia::detail::validate_migration_list;
    const db_migration ok[] = {
        db_migration{{.id_ = "001_init", .sql_ = "CREATE TABLE a(id INT)"}},
        db_migration{{.id_ = "002_more", .sql_ = "ALTER TABLE a ADD b INT"}},
    };
    RUVIA_CHECK(!throws_on([&] { validate_migration_list(std::span<const db_migration>(ok, 2)); }));
    // An empty list is valid: nothing to apply.
    RUVIA_CHECK(!throws_on([&] { validate_migration_list(std::span<const db_migration>()); }));
    // Duplicate ids would apply the wrong migration -> rejected.
    const db_migration dup[] = {
        db_migration{{.id_ = "001", .sql_ = "SQL1"}},
        db_migration{{.id_ = "001", .sql_ = "SQL2"}},
    };
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(dup, 2)); }));
    // Empty id and empty SQL are rejected.
    const db_migration empty_id[] = {db_migration{{.id_ = "", .sql_ = "SQL"}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(empty_id, 1)); }));
    const db_migration empty_sql[] = {db_migration{{.id_ = "001", .sql_ = ""}}};
    RUVIA_CHECK(
        throws_on([&] { validate_migration_list(std::span<const db_migration>(empty_sql, 1)); }));
    const db_migration blank_sql[] = {db_migration{{.id_ = "001", .sql_ = " \n\t\r"}}};
    RUVIA_CHECK(
        throws_on([&] { validate_migration_list(std::span<const db_migration>(blank_sql, 1)); }));
    const db_migration comment_only_sql[] = {
        db_migration{{.id_ = "001", .sql_ = "/* no statement */\n-- still none\n;"}}};
    RUVIA_CHECK(
        throws_on([&] { validate_migration_list(std::span<const db_migration>(comment_only_sql, 1)); }));
    const db_migration nested_comment_only_sql[] = {db_migration{{.id_ = "001",
        .sql_ = "/* outer /* inner */ still comment */"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(std::span<const db_migration>(nested_comment_only_sql, 1));
    }));
    // An id longer than the 190-byte schema column is rejected.
    const std::string long_id(191, 'x');
    const db_migration too_long[] = {db_migration{{.id_ = long_id, .sql_ = "SQL"}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(too_long, 1)); }));

    // Ids that differ only in letter case are one id to a case-insensitive
    // collation: MariaDB's default would report the second as already applied
    // and never run it, while PostgreSQL would apply both. Refused here so one
    // list cannot produce two schemas.
    const db_migration case_dup[] = {
        db_migration{{.id_ = "v1_users", .sql_ = "SQL1"}},
        db_migration{{.id_ = "V1_Users", .sql_ = "SQL2"}},
    };
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(case_dup, 2)); }));
    // Ids that differ in more than case remain distinct.
    const db_migration distinct[] = {
        db_migration{{.id_ = "v1_users", .sql_ = "SQL1"}},
        db_migration{{.id_ = "v2_users", .sql_ = "SQL2"}},
    };
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(distinct, 2)); }));

    // MariaDB collations are PAD SPACE -- the binary one the table pins
    // included -- so "v1" and "v1 " would be one row there and two on
    // PostgreSQL. A surrounded id is refused rather than folded.
    const db_migration padded[] = {db_migration{{.id_ = "v1_users ", .sql_ = "SQL1"}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(padded, 1)); }));
    const db_migration leading[] = {db_migration{{.id_ = " v1_users", .sql_ = "SQL1"}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(leading, 1)); }));
    // Interior spaces are not the ambiguity; they compare exactly.
    const db_migration interior[] = {db_migration{{.id_ = "v1 users", .sql_ = "SQL1"}}};
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(interior, 1)); }));
}

RUVIA_TEST(db_migration_list_validation_enforces_one_statement) {
    using ruvia::db_driver;
    using ruvia::db_migration;
    using ruvia::detail::validate_migration_list;

    // Neither backend runs two statements in one call, so the packaging error
    // is reported here instead of arriving as a backend syntax error pointing
    // at the second statement.
    const db_migration two[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT); CREATE TABLE b(id INT)"}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(two, 1)); }));

    // A trailing separator is accepted by both backends, so it is accepted
    // here -- with or without trailing whitespace.
    const db_migration trailing[] = {db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT);"}}};
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(trailing, 1)); }));
    const db_migration trailing_space[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT);\n  "}}};
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(trailing_space, 1)); }));
    const db_migration trailing_line_comment[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT); -- one statement\n"}}};
    RUVIA_CHECK(!throws_on(
        [&] { validate_migration_list(std::span<const db_migration>(trailing_line_comment, 1)); }));
    const db_migration trailing_block_comment[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT); /* one; statement */"}}};
    RUVIA_CHECK(!throws_on(
        [&] { validate_migration_list(std::span<const db_migration>(trailing_block_comment, 1)); }));
    const db_migration trailing_nested_block_comment[] = {db_migration{{.id_ = "001",
        .sql_ = "SELECT 1; /* outer /* nested */ still comment */"}}};
    RUVIA_CHECK(!throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(trailing_nested_block_comment, 1), db_driver::postgresql);
    }));

    // A ';' that is data -- inside a default value, a quoted identifier or a
    // comment -- is not a statement separator.
    const db_migration quoted[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT, s VARCHAR(4) DEFAULT 'a;b')"}}};
    RUVIA_CHECK(!throws_on([&] { validate_migration_list(std::span<const db_migration>(quoted, 1)); }));
    const db_migration commented[] = {
        db_migration{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT) -- one; two\n"}}};
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(commented, 1)); }));

    // PostgreSQL DO blocks and function bodies routinely contain statement
    // separators inside dollar-quoted text. Those bytes are part of the one DO
    // or CREATE FUNCTION statement, including when tags are nested.
    const db_migration dollar_quoted[] = {
        db_migration{{.id_ = "001", .sql_ = "DO $$ BEGIN PERFORM 1; PERFORM 2; END $$;"}}};
    RUVIA_CHECK(
        !throws_on([&] { validate_migration_list(std::span<const db_migration>(dollar_quoted, 1)); }));
    const db_migration tagged[] = {db_migration{{.id_ = "001",
        .sql_ = "DO $schema$ BEGIN EXECUTE $body$ SELECT 1; SELECT 2 $body$; END $schema$;"}}};
    RUVIA_CHECK(!throws_on([&] { validate_migration_list(std::span<const db_migration>(tagged, 1)); }));

    // Dollar signs belong to an unquoted PostgreSQL identifier when the
    // would-be opening tag is adjacent to the identifier's first byte.
    const db_migration identifier_tags[] = {db_migration{{.id_ = "001",
        .sql_ = "SELECT 1 AS foo$tag$; SELECT 2 AS bar$tag$;"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(std::span<const db_migration>(identifier_tags, 1), db_driver::postgresql);
    }));

    // PostgreSQL ordinary strings and quoted identifiers do not use a
    // backslash to escape the closing delimiter. These are therefore two
    // statements to PostgreSQL even though MariaDB-style scanning used to
    // consume everything up to the final quote and miss the separator.
    const db_migration pg_backslash_string_split[] = {
        db_migration{{.id_ = "001", .sql_ = R"(SELECT 'a\'; SELECT 2; --')"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(pg_backslash_string_split, 1), db_driver::postgresql);
    }));
    const db_migration pg_backslash_identifier_split[] = {
        db_migration{{.id_ = "001", .sql_ = R"(SELECT "a\"; SELECT 2; --")"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(pg_backslash_identifier_split, 1), db_driver::postgresql);
    }));
    const db_migration pg_hash_operator_split[] = {
        db_migration{{.id_ = "001", .sql_ = "SELECT 1 # 2; SELECT 3"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(pg_hash_operator_split, 1), db_driver::postgresql);
    }));

    // The MariaDB validator still accepts the constructs that are data there:
    // backslash-escaped quotes inside strings and '#' line comments.
    const db_migration maria_backslash_string[] = {
        db_migration{{.id_ = "001", .sql_ = R"(SELECT 'a\'; SELECT 2; --')"}}};
    RUVIA_CHECK(!throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(maria_backslash_string, 1), db_driver::mariadb);
    }));
    const db_migration maria_hash_comment[] = {
        db_migration{{.id_ = "001", .sql_ = "SELECT 1 # one; two\n"}}};
    RUVIA_CHECK(!throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(maria_hash_comment, 1), db_driver::mariadb);
    }));
    const db_migration maria_trailing_hash_comment[] = {
        db_migration{{.id_ = "001", .sql_ = "SELECT 1; # one statement\n"}}};
    RUVIA_CHECK(!throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(maria_trailing_hash_comment, 1), db_driver::mariadb);
    }));
    const db_migration maria_dash_no_whitespace[] = {
        db_migration{{.id_ = "001", .sql_ = "SELECT 1; --not a MariaDB comment"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(maria_dash_no_whitespace, 1), db_driver::mariadb);
    }));
    const db_migration pg_trailing_hash_text[] = {
        db_migration{{.id_ = "001", .sql_ = "SELECT 1; # not a PostgreSQL comment"}}};
    RUVIA_CHECK(throws_on([&] {
        validate_migration_list(
            std::span<const db_migration>(pg_trailing_hash_text, 1), db_driver::postgresql);
    }));
}

RUVIA_TEST(db_migration_checksum_pins_the_recorded_text) {
    using ruvia::detail::migration_checksum;
    using ruvia::detail::migration_checksum_size;

    // The published SHA-256 vector for "abc", lowercase hex: a stored checksum
    // has to keep meaning the same thing across releases, so the digest and its
    // encoding are pinned rather than merely self-consistent.
    const auto abc = migration_checksum("abc", std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string(abc.data(), abc.size()),
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    RUVIA_CHECK_EQ(abc.size(), migration_checksum_size);

    // Same text, same digest; one edited byte, a different one.
    const auto first =
        migration_checksum("CREATE TABLE a(id INT)", std::pmr::get_default_resource());
    const auto again =
        migration_checksum("CREATE TABLE a(id INT)", std::pmr::get_default_resource());
    const auto edited =
        migration_checksum("CREATE TABLE a(id BIGINT)", std::pmr::get_default_resource());
    RUVIA_CHECK(first == again);
    RUVIA_CHECK(first != edited);
}

RUVIA_TEST(db_migration_carries_its_atomicity) {
    using ruvia::db_migration;
    using ruvia::db_migration_atomicity;

    // Committing the statement and the row that records it together is the
    // default; naming the exception is opt-in and per migration, so one
    // statement that cannot run in a transaction block does not cost the rest
    // of the list its atomicity.
    const db_migration standard{{.id_ = "001", .sql_ = "CREATE TABLE a(id INT)"}};
    const db_migration concurrent{{.id_ = "002",
        .sql_ = "CREATE INDEX CONCURRENTLY i ON a (id)",
        .atomicity_ = db_migration_atomicity::unwrapped}};
    RUVIA_CHECK(standard.atomicity() == db_migration_atomicity::transactional);
    RUVIA_CHECK(concurrent.atomicity() == db_migration_atomicity::unwrapped);
}

RUVIA_TEST(db_migration_descriptor_owns_and_exposes_public_options) {
    using ruvia::db_migration;
    using ruvia::db_migration_atomicity;
    using ruvia::db_migration_options;

    std::string id = "cs_owned";
    std::string sql = "CREATE TABLE cs_owned (id INTEGER)";
    db_migration migration(db_migration_options{.id_ = std::move(id),
        .sql_ = std::move(sql),
        .atomicity_ = db_migration_atomicity::unwrapped});
    RUVIA_CHECK_EQ(migration.id(), "cs_owned");
    RUVIA_CHECK_EQ(migration.sql(), "CREATE TABLE cs_owned (id INTEGER)");
    RUVIA_CHECK_EQ(migration.atomicity(), db_migration_atomicity::unwrapped);

    const db_migration defaults(db_migration_options{.id_ = "cs_default", .sql_ = "SELECT 1"});
    RUVIA_CHECK_EQ(defaults.atomicity(), db_migration_atomicity::transactional);
}

RUVIA_TEST(db_migration_list_rejects_invalid_atomicity) {
    using ruvia::db_migration;
    using ruvia::db_migration_atomicity;
    using ruvia::detail::validate_migration_list;

    const std::array migrations{db_migration{{.id_ = "001",
        .sql_ = "CREATE TABLE a(id INT)",
        .atomicity_ = static_cast<db_migration_atomicity>(42)}}};
    RUVIA_CHECK(throws_on([&] { validate_migration_list(std::span<const db_migration>(migrations)); }));
}

RUVIA_TEST(db_sql_scan_steps_over_opaque_constructs) {
    using ruvia::detail::find_postgresql_syntax_byte;
    using ruvia::detail::find_sql_syntax_byte;
    using ruvia::detail::skip_postgresql_dollar_quoted_atom;
    using ruvia::detail::skip_sql_atom;

    // The scan is shared by the parameter binder and the migration validator,
    // so its own boundaries are pinned here.
    RUVIA_CHECK_EQ(skip_sql_atom("abc", 0), std::size_t{1});
    RUVIA_CHECK_EQ(skip_sql_atom("'ab'x", 0), std::size_t{4});
    RUVIA_CHECK_EQ(skip_sql_atom("'a''b'x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skip_sql_atom("`a``b`x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skip_sql_atom("-- c\nx", 0), std::size_t{5});
    RUVIA_CHECK_EQ(skip_sql_atom("--not comment", 0), std::size_t{1});
    RUVIA_CHECK_EQ(skip_sql_atom("/* c */x", 0), std::size_t{7});
    // A backslash escapes inside quotes but never inside a quoted identifier,
    // where MariaDB treats it as an ordinary byte.
    RUVIA_CHECK_EQ(skip_sql_atom("'a\\'b'x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skip_sql_atom("`a\\`x", 0), std::size_t{4});
    // An unterminated construct consumes the rest rather than running past the
    // end; the caller then reports a mismatch instead of binding into it.
    RUVIA_CHECK_EQ(skip_sql_atom("'abc", 0), std::size_t{4});
    RUVIA_CHECK_EQ(skip_sql_atom("/* abc", 0), std::size_t{6});
    // A lone '-' or '/' is an operator, not a comment.
    RUVIA_CHECK_EQ(skip_sql_atom("a-b", 1), std::size_t{2});
    RUVIA_CHECK_EQ(skip_sql_atom("a/b", 1), std::size_t{2});

    RUVIA_CHECK_EQ(find_sql_syntax_byte("a;b", ';'), std::size_t{1});
    RUVIA_CHECK_EQ(find_sql_syntax_byte("'a;b'", ';'), std::string_view::npos);
    RUVIA_CHECK_EQ(find_sql_syntax_byte("'a;b';", ';'), std::size_t{5});
    RUVIA_CHECK_EQ(find_sql_syntax_byte("x", '?'), std::string_view::npos);
    RUVIA_CHECK_EQ(find_sql_syntax_byte("SELECT 1--?", '?'), std::size_t{10});

    RUVIA_CHECK_EQ(skip_postgresql_dollar_quoted_atom("$$a;b$$x", 0), std::size_t{7});
    RUVIA_CHECK_EQ(skip_postgresql_dollar_quoted_atom("$tag$a;b$tag$x", 0), std::size_t{13});
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte("DO $$a;b$$;", ';'), std::size_t{10});
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte("DO $tag$a;b$tag$; SELECT 2", ';'), std::size_t{16});
    // A positional parameter is not a dollar-quote opener.
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte("SELECT $1; SELECT 2", ';'), std::size_t{9});
    // PostgreSQL's standard quoted strings and quoted identifiers do not make
    // a backslash escape the delimiter; E'...' strings do.
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte(R"(SELECT 'a\'; SELECT 2; --')", ';'), std::size_t{11});
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte(R"(SELECT "a\"; SELECT 2; --")", ';'), std::size_t{11});
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte(R"(SELECT E'a\';b'; SELECT 2)", ';'), std::size_t{15});
    RUVIA_CHECK_EQ(find_postgresql_syntax_byte("SELECT 1 # 2; SELECT 3", ';'), std::size_t{12});
    RUVIA_CHECK_EQ(find_sql_syntax_byte("SELECT 1 # one; two\nSELECT 2", ';'), std::string_view::npos);
}
