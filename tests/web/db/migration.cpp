// Public migration descriptors and options are shared by both SQL backends.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_schema.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "failing_memory_resource.h"

#if defined(RUVIA_ENABLE_POSTGRESQL) || defined(RUVIA_ENABLE_MARIADB)
namespace {

void check_unverifiable_history(
    ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver,
    std::span<const ruvia::db_migration> migrations) {
    auto config = peer.config();
    config.driver_ = driver;
    config.username_ = "test";
    config.query_timeout_ = std::chrono::seconds(2);
    failing_memory_resource resource;
    bool rejected = false;
    try {
        (void)ruvia::db_migrator::migrate(config, migrations, {.resource_ = &resource});
    } catch (const ruvia::db_error&) {
        // A scripted wire error is not evidence of history validation.
        throw;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

}  // namespace
#endif

TEST(Ruvia, db_migrator_rejects_sql_with_embedded_nul) {
    bool nul_sql_rejected = false;
    try {
        const std::array<ruvia::db_migration, 1> bad_migrations{{
            ruvia::db_migration{{.id_ = "001", .sql_ = std::string("SELECT 1\0", 9)}},
        }};
        (void)ruvia::db_migrator::migrate(
            ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}, bad_migrations);
    } catch (const std::invalid_argument&) {
        nul_sql_rejected = true;
    }
    EXPECT_TRUE(nul_sql_rejected);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_migrator_rejects_unverifiable_postgresql_history) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE t(id INT)"})};
    for (const bool null : {false, true}) {
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
            {{.column_ = "", .command_ = "SET"},
                {.column_ = "pg_advisory_lock", .rows_ = {""}},
                {.column_ = "", .command_ = "RESET"},
                {.column_ = "", .command_ = "CREATE TABLE"},
                {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}},
                {.column_ = "checksum", .rows_ = {""}, .null_rows_ = {null}},
                {.column_ = "", .command_ = "UPDATE 1"},
                {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}});
        check_unverifiable_history(peer, ruvia::db_driver::postgresql, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_postgresql_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.column_ = "", .command_ = "SET"},
            {.column_ = "pg_advisory_lock", .rows_ = {""}},
            {.column_ = "", .command_ = "RESET"},
            {.column_ = "", .command_ = "CREATE TABLE"},
            {.column_ = "?column?", .type_ = 23},
            {.column_ = "", .command_ = "ALTER TABLE"},
            {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}});
    check_unverifiable_history(peer, ruvia::db_driver::postgresql, {});
}

// The migration lock belongs to the session that took it. A migration that
// leaves that session unfit for sharing -- here an unwrapped BEGIN leaving a
// transaction open -- ends it, and the history row must then fail rather than
// be written on a new session that never held the lock.
TEST(Ruvia, db_migrator_fails_once_its_locked_session_is_lost) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration(
        {.id_ = "001", .sql_ = "BEGIN", .atomicity_ = ruvia::db_migration_atomicity::unwrapped})};
    peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>{
        {{.column_ = "", .command_ = "SET"},
            {.column_ = "pg_advisory_lock", .rows_ = {""}},
            {.column_ = "", .command_ = "RESET"},
            {.column_ = "", .command_ = "CREATE TABLE"},
            {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}},
            {.column_ = "checksum"},
            {.column_ = "", .command_ = "BEGIN", .status_ = 'T'}},
        // A replacement session, which never took the lock.
        {{.column_ = "", .command_ = "INSERT 0 1"},
            {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}}});
    auto config = peer.config();
    config.username_ = "test";
    config.query_timeout_ = std::chrono::seconds(2);
    std::optional<ruvia::db_error::code_type> failure;
    try {
        (void)ruvia::db_migrator::migrate(config, migrations);
    } catch (const ruvia::db_error& error) {
        failure = error.code();
    }
    EXPECT_TRUE(failure == ruvia::db_error::code_type::io_error);
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// The migrator drives its own event loop, so it also brings the pool that loop
// looks a DNS host up on: a name that cannot resolve (RFC 6761 §6.4) fails the
// lookup, not the configuration.
TEST(Ruvia, db_migrator_looks_up_a_dns_host_on_its_own_pool) {
    ruvia::db_config config{.driver_ = ruvia::db_driver::postgresql,
        .host_ = "ruvia-migrator.invalid",
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
    config.connect_timeout_ = std::chrono::seconds(1);
    bool lookup_failed = false;
    try {
        (void)ruvia::db_migrator::migrate(config, std::span<const ruvia::db_migration>{});
    } catch (const ruvia::db_error&) {
        lookup_failed = true;
    }
    EXPECT_TRUE(lookup_failed);
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, db_migrator_rejects_unverifiable_mariadb_history) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE t(id INT)"})};
    for (const bool null : {false, true}) {
        peer_type peer(std::vector<peer_type::maria_reply>{
                           {.rows_ = {"1"}},
                           {.column_ = ""},
                           {.rows_ = {"1"}},
                           {.column_ = "checksum", .rows_ = {""}, .null_rows_ = {null}},
                           {.column_ = "", .affected_rows_ = 1},
                           {.column_ = ""}},
            peer_type::after_startup::idle);
        check_unverifiable_history(peer, ruvia::db_driver::mariadb, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_mariadb_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(std::vector<peer_type::maria_reply>{
                       {.rows_ = {"1"}},
                       {.column_ = ""},
                       {},
                       {.column_ = ""},
                       {.column_ = ""}},
        peer_type::after_startup::idle);
    check_unverifiable_history(peer, ruvia::db_driver::mariadb, {});
}
#endif
