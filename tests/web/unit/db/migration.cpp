// Migration bookkeeping is backend-neutral: id and SQL validation, the digest
// recorded with an applied migration, and the scan both drivers use to tell
// statement syntax from data. Nothing here needs a driver's client library, so
// it is compiled for either of them rather than only alongside MariaDB.

#include "test_harness.h"

#include <array>
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
