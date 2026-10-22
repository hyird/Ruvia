// Public migration descriptors and options are shared by both SQL backends.

#include <array>
#include <chrono>
#include <span>
#include <string_view>

#include "ruvia/web/db/db_schema.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
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
    bool nul_sql_rejected = false;
    try {
        const std::array<db_migration, 1> bad_migrations{{
            db_migration{{.id_ = "001", .sql_ = std::string("SELECT 1\0", 9)}},
        }};
        (void)ruvia::db_migrator::migrate(
            ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}, bad_migrations);
    } catch (const std::invalid_argument&) {
        nul_sql_rejected = true;
    }
    RUVIA_CHECK(nul_sql_rejected);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_migrator_lock_timeout_bounds_only_the_postgresql_lock_wait) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.column_ = "", .command_ = "SET"},
            {.column_ = "pg_advisory_lock", .rows_ = {""}},
            {.column_ = "", .command_ = "RESET"},
            {.column_ = "", .command_ = "CREATE TABLE"},
            {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}},
            {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}});
    auto config = peer.config();
    config.username_ = "test";
    config.pool_size_per_worker_ = 3;
    config.query_timeout_ = std::chrono::seconds(5);
    const auto report = ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>{});
    RUVIA_CHECK(!report.changed());
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{6});
    if (statements.size() == 6) {
        RUVIA_CHECK(statements[0].sql_.starts_with("SET lock_timeout TO '30000ms'"));
        RUVIA_CHECK(statements[1].sql_.starts_with("SELECT pg_advisory_lock("));
        // The session timeout must not outlive the lock wait: every later
        // statement, including each migration, runs with the server default.
        RUVIA_CHECK(statements[2].sql_ == "RESET lock_timeout");
        RUVIA_CHECK(statements[3].sql_.starts_with("CREATE TABLE IF NOT EXISTS"));
    }
}
#endif
