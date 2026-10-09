// Public migration descriptors and options are shared by both SQL backends.

#include <array>
#include <string_view>

#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/db/db_types.h"

#include "test_harness.h"

RUVIA_TEST(db_migrator_options_default_table_uses_ruvia_name) {
    const ruvia::db_migrator_options options;
    RUVIA_CHECK(options.table_ == "ruvia_schema_migrations");
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
