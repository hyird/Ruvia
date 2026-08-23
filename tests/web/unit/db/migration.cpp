// Migration bookkeeping is backend-neutral: id and SQL validation, the digest
// recorded with an applied migration, and the scan both drivers use to tell
// statement syntax from data. Nothing here needs a driver's client library, so
// it is compiled for either of them rather than only alongside MariaDB.

#include "test_harness.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbMigrationChecksum.h"
#include "ruvia/web/detail/db/DbMigrationValidation.h"
#include "ruvia/web/detail/db/DbSqlScan.h"

namespace {

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

RUVIA_TEST(db_migrator_options_default_table_uses_ruvia_name) {
    const ruvia::DbMigratorOptions options;
    RUVIA_CHECK(options.table == "ruvia_schema_migrations");
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

RUVIA_TEST(db_migration_postgresql_lock_timeout_conversion_is_checked) {
    using ruvia::detail::postgresLockTimeoutMilliseconds;

    const auto largestRepresentableSeconds = std::chrono::seconds(std::chrono::milliseconds::max().count() / 1000);
    RUVIA_CHECK_EQ(postgresLockTimeoutMilliseconds(largestRepresentableSeconds), static_cast<std::uint64_t>(largestRepresentableSeconds.count()) * 1000U);

    bool overflowRejected = false;
    try {
        (void)postgresLockTimeoutMilliseconds(std::chrono::seconds::max());
    } catch (const std::invalid_argument&) {
        overflowRejected = true;
    }
    RUVIA_CHECK(overflowRejected);
}

RUVIA_TEST(db_migration_list_validation_enforces_integrity) {
    using ruvia::DbMigration;
    using ruvia::detail::validateMigrationList;
    const DbMigration ok[] = {
        DbMigration{{.id = "001_init", .sql = "CREATE TABLE a(id INT)"}},
        DbMigration{{.id = "002_more", .sql = "ALTER TABLE a ADD b INT"}},
    };
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(ok, 2)); }));
    // An empty list is valid: nothing to apply.
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>()); }));
    // Duplicate ids would apply the wrong migration -> rejected.
    const DbMigration dup[] = {
        DbMigration{{.id = "001", .sql = "SQL1"}},
        DbMigration{{.id = "001", .sql = "SQL2"}},
    };
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(dup, 2)); }));
    // Empty id and empty SQL are rejected.
    const DbMigration emptyId[] = {DbMigration{{.id = "", .sql = "SQL"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(emptyId, 1)); }));
    const DbMigration emptySql[] = {DbMigration{{.id = "001", .sql = ""}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(emptySql, 1)); }));
    const DbMigration blankSql[] = {DbMigration{{.id = "001", .sql = " \n\t\r"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(blankSql, 1)); }));
    const DbMigration commentOnlySql[] = {DbMigration{{.id = "001", .sql = "/* no statement */\n-- still none\n;"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(commentOnlySql, 1)); }));
    // An id longer than the 190-byte schema column is rejected.
    const std::string longId(191, 'x');
    const DbMigration tooLong[] = {DbMigration{{.id = longId, .sql = "SQL"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(tooLong, 1)); }));

    // Ids that differ only in letter case are one id to a case-insensitive
    // collation: MariaDB's default would report the second as already applied
    // and never run it, while PostgreSQL would apply both. Refused here so one
    // list cannot produce two schemas.
    const DbMigration caseDup[] = {
        DbMigration{{.id = "v1_users", .sql = "SQL1"}},
        DbMigration{{.id = "V1_Users", .sql = "SQL2"}},
    };
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(caseDup, 2)); }));
    // Ids that differ in more than case remain distinct.
    const DbMigration distinct[] = {
        DbMigration{{.id = "v1_users", .sql = "SQL1"}},
        DbMigration{{.id = "v2_users", .sql = "SQL2"}},
    };
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(distinct, 2)); }));

    // MariaDB collations are PAD SPACE -- the binary one the table pins
    // included -- so "v1" and "v1 " would be one row there and two on
    // PostgreSQL. A surrounded id is refused rather than folded.
    const DbMigration padded[] = {DbMigration{{.id = "v1_users ", .sql = "SQL1"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(padded, 1)); }));
    const DbMigration leading[] = {DbMigration{{.id = " v1_users", .sql = "SQL1"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(leading, 1)); }));
    // Interior spaces are not the ambiguity; they compare exactly.
    const DbMigration interior[] = {DbMigration{{.id = "v1 users", .sql = "SQL1"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(interior, 1)); }));
}

RUVIA_TEST(db_migration_list_validation_enforces_one_statement) {
    using ruvia::DbDriver;
    using ruvia::DbMigration;
    using ruvia::detail::validateMigrationList;

    // Neither backend runs two statements in one call, so the packaging error
    // is reported here instead of arriving as a backend syntax error pointing
    // at the second statement.
    const DbMigration two[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT); CREATE TABLE b(id INT)"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(two, 1)); }));

    // A trailing separator is accepted by both backends, so it is accepted
    // here -- with or without trailing whitespace.
    const DbMigration trailing[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT);"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailing, 1)); }));
    const DbMigration trailingSpace[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT);\n  "}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailingSpace, 1)); }));
    const DbMigration trailingLineComment[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT); -- one statement\n"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailingLineComment, 1)); }));
    const DbMigration trailingBlockComment[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT); /* one; statement */"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(trailingBlockComment, 1)); }));

    // A ';' that is data -- inside a default value, a quoted identifier or a
    // comment -- is not a statement separator.
    const DbMigration quoted[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT, s VARCHAR(4) DEFAULT 'a;b')"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(quoted, 1)); }));
    const DbMigration commented[] = {DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT) -- one; two\n"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(commented, 1)); }));

    // PostgreSQL DO blocks and function bodies routinely contain statement
    // separators inside dollar-quoted text. Those bytes are part of the one DO
    // or CREATE FUNCTION statement, including when tags are nested.
    const DbMigration dollarQuoted[] = {DbMigration{{.id = "001", .sql = "DO $$ BEGIN PERFORM 1; PERFORM 2; END $$;"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(dollarQuoted, 1)); }));
    const DbMigration tagged[] = {DbMigration{{.id = "001", .sql = "DO $schema$ BEGIN EXECUTE $body$ SELECT 1; SELECT 2 $body$; END $schema$;"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(tagged, 1)); }));

    // PostgreSQL ordinary strings and quoted identifiers do not use a
    // backslash to escape the closing delimiter. These are therefore two
    // statements to PostgreSQL even though MariaDB-style scanning used to
    // consume everything up to the final quote and miss the separator.
    const DbMigration pgBackslashStringSplit[] = {DbMigration{{.id = "001", .sql = R"(SELECT 'a\'; SELECT 2; --')"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(pgBackslashStringSplit, 1), DbDriver::kPostgreSql); }));
    const DbMigration pgBackslashIdentifierSplit[] = {DbMigration{{.id = "001", .sql = R"(SELECT "a\"; SELECT 2; --")"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(pgBackslashIdentifierSplit, 1), DbDriver::kPostgreSql); }));
    const DbMigration pgHashOperatorSplit[] = {DbMigration{{.id = "001", .sql = "SELECT 1 # 2; SELECT 3"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(pgHashOperatorSplit, 1), DbDriver::kPostgreSql); }));

    // The MariaDB validator still accepts the constructs that are data there:
    // backslash-escaped quotes inside strings and '#' line comments.
    const DbMigration mariaBackslashString[] = {DbMigration{{.id = "001", .sql = R"(SELECT 'a\'; SELECT 2; --')"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(mariaBackslashString, 1), DbDriver::kMariaDb); }));
    const DbMigration mariaHashComment[] = {DbMigration{{.id = "001", .sql = "SELECT 1 # one; two\n"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(mariaHashComment, 1), DbDriver::kMariaDb); }));
    const DbMigration mariaTrailingHashComment[] = {DbMigration{{.id = "001", .sql = "SELECT 1; # one statement\n"}}};
    RUVIA_CHECK(!throwsOn([&] { validateMigrationList(std::span<const DbMigration>(mariaTrailingHashComment, 1), DbDriver::kMariaDb); }));
    const DbMigration mariaDashNoWhitespace[] = {DbMigration{{.id = "001", .sql = "SELECT 1; --not a MariaDB comment"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(mariaDashNoWhitespace, 1), DbDriver::kMariaDb); }));
    const DbMigration pgTrailingHashText[] = {DbMigration{{.id = "001", .sql = "SELECT 1; # not a PostgreSQL comment"}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(pgTrailingHashText, 1), DbDriver::kPostgreSql); }));
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
    const DbMigration standard{{.id = "001", .sql = "CREATE TABLE a(id INT)"}};
    const DbMigration concurrent{{.id = "002", .sql = "CREATE INDEX CONCURRENTLY i ON a (id)", .atomicity = DbMigrationAtomicity::kUnwrapped}};
    RUVIA_CHECK(standard.atomicity() == DbMigrationAtomicity::kTransactional);
    RUVIA_CHECK(concurrent.atomicity() == DbMigrationAtomicity::kUnwrapped);
}

RUVIA_TEST(db_migration_list_rejects_invalid_atomicity) {
    using ruvia::DbMigration;
    using ruvia::DbMigrationAtomicity;
    using ruvia::detail::validateMigrationList;

    const std::array migrations{DbMigration{{.id = "001", .sql = "CREATE TABLE a(id INT)", .atomicity = static_cast<DbMigrationAtomicity>(42)}}};
    RUVIA_CHECK(throwsOn([&] { validateMigrationList(std::span<const DbMigration>(migrations)); }));
}

RUVIA_TEST(db_sql_scan_steps_over_opaque_constructs) {
    using ruvia::detail::findPostgreSqlSyntaxByte;
    using ruvia::detail::findSqlSyntaxByte;
    using ruvia::detail::skipPostgreSqlDollarQuotedAtom;
    using ruvia::detail::skipSqlAtom;

    // The scan is shared by the parameter binder and the migration validator,
    // so its own boundaries are pinned here.
    RUVIA_CHECK_EQ(skipSqlAtom("abc", 0), std::size_t{1});
    RUVIA_CHECK_EQ(skipSqlAtom("'ab'x", 0), std::size_t{4});
    RUVIA_CHECK_EQ(skipSqlAtom("'a''b'x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skipSqlAtom("`a``b`x", 0), std::size_t{6});
    RUVIA_CHECK_EQ(skipSqlAtom("-- c\nx", 0), std::size_t{5});
    RUVIA_CHECK_EQ(skipSqlAtom("--not comment", 0), std::size_t{1});
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
    RUVIA_CHECK_EQ(findSqlSyntaxByte("SELECT 1--?", '?'), std::size_t{10});

    RUVIA_CHECK_EQ(skipPostgreSqlDollarQuotedAtom("$$a;b$$x", 0), std::size_t{7});
    RUVIA_CHECK_EQ(skipPostgreSqlDollarQuotedAtom("$tag$a;b$tag$x", 0), std::size_t{13});
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte("DO $$a;b$$;", ';'), std::size_t{10});
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte("DO $tag$a;b$tag$; SELECT 2", ';'), std::size_t{16});
    // A positional parameter is not a dollar-quote opener.
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte("SELECT $1; SELECT 2", ';'), std::size_t{9});
    // PostgreSQL's standard quoted strings and quoted identifiers do not make
    // a backslash escape the delimiter; E'...' strings do.
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte(R"(SELECT 'a\'; SELECT 2; --')", ';'), std::size_t{11});
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte(R"(SELECT "a\"; SELECT 2; --")", ';'), std::size_t{11});
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte(R"(SELECT E'a\';b'; SELECT 2)", ';'), std::size_t{15});
    RUVIA_CHECK_EQ(findPostgreSqlSyntaxByte("SELECT 1 # 2; SELECT 3", ';'), std::size_t{12});
    RUVIA_CHECK_EQ(findSqlSyntaxByte("SELECT 1 # one; two\nSELECT 2", ';'), std::string_view::npos);
}
