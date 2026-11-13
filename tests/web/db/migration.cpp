// Public migration descriptors and options are shared by both SQL backends.

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/web/db/db_schema.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "db_migration_fixture.h"

#if defined(RUVIA_ENABLE_POSTGRESQL) || defined(RUVIA_ENABLE_MARIADB)
namespace {

void check_unverifiable_history(
    ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver,
    std::span<const ruvia::db_migration> migrations) {
    auto config = peer.config();
    config.driver_ = driver;
    config.username_ = "test";
    config.query_timeout_ = std::chrono::seconds(2);
    bool rejected = false;
    try {
        (void)ruvia::test::run_db_migration(config, migrations);
    } catch (const ruvia::db_error&) {
        // A scripted wire error is not evidence of history validation.
        throw;
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

[[nodiscard]] ruvia::db_config scripted_config(ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver) {
    auto config = peer.config();
    config.driver_ = driver;
    config.username_ = "test";
    return config;
}

// Starts one run, waits until the peer holds its lock request, then stops it
// through `stop` (when given) and returns how the run ended. The peer never
// answers the lock, so only the run's own operation can end it.
template <typename lock_requested_type>
[[nodiscard]] std::optional<ruvia::db_error::code_type> interrupted_migration(const ruvia::db_config& config,
    ruvia::operation_options operation, ruvia::stop_source* stop, lock_requested_type lock_requested) {
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE t(id INT)"})};
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const ruvia::db_migrator migrator(loops.loop(0), config);
    auto migration = migrator.migrate(migrations, std::move(operation));
    loops.start();
    auto run = loops.loop(0).start(std::move(migration));
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!lock_requested() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(lock_requested());
    if (stop != nullptr) {
        stop->request_stop();
    }
    EXPECT_TRUE(run.wait_for(std::chrono::seconds(5)));
    loops.join();
    try {
        (void)std::move(run).get();
    } catch (const ruvia::db_error& error) {
        return error.code();
    }
    return std::nullopt;
}

}  // namespace
#endif

TEST(Ruvia, db_migrator_rejects_sql_with_embedded_nul) {
    bool nul_sql_rejected = false;
    try {
        const std::array<ruvia::db_migration, 1> bad_migrations{{
            ruvia::db_migration{{.id_ = "001", .sql_ = std::string("SELECT 1\0", 9)}},
        }};
        (void)ruvia::test::run_db_migration(
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
            std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("SET")},
                {.column_ = std::string("pg_advisory_lock"), .rows_ = std::vector<std::string>({std::string("")})},
                {.column_ = std::string(""), .command_ = std::string("RESET")},
                {.column_ = std::string(""), .command_ = std::string("CREATE TABLE")},
                {.column_ = std::string("?column?"), .type_ = 23, .rows_ = std::vector<std::string>({std::string("1")})},
                {.column_ = std::string("checksum"), .rows_ = std::vector<std::string>({std::string("")}), .null_rows_ = std::vector<bool>(1, null)},
                {.column_ = std::string(""), .command_ = std::string("UPDATE 1")},
                {.column_ = std::string("pg_advisory_unlock"), .type_ = 16, .rows_ = std::vector<std::string>({std::string("t")})}}));
        check_unverifiable_history(peer, ruvia::db_driver::postgresql, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_postgresql_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("SET")},
            {.column_ = std::string("pg_advisory_lock"), .rows_ = std::vector<std::string>({std::string("")})},
            {.column_ = std::string(""), .command_ = std::string("RESET")},
            {.column_ = std::string(""), .command_ = std::string("CREATE TABLE")},
            {.column_ = std::string("?column?"), .type_ = 23},
            {.column_ = std::string(""), .command_ = std::string("ALTER TABLE")},
            {.column_ = std::string("pg_advisory_unlock"), .type_ = 16, .rows_ = std::vector<std::string>({std::string("t")})}}));
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
    peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>({std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"},
                                                                            {.column_ = "pg_advisory_lock", .rows_ = {""}},
                                                                            {.column_ = "", .command_ = "RESET"},
                                                                            {.column_ = "", .command_ = "CREATE TABLE"},
                                                                            {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}},
                                                                            {.column_ = "checksum"},
                                                                            {.column_ = "", .command_ = "BEGIN", .status_ = 'T'}}),
        std::vector<peer_type::postgres_reply>(  // A replacement session, which never took the lock.
            {{.column_ = "", .command_ = "INSERT 0 1"},
                {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}})}));
    auto config = peer.config();
    config.username_ = "test";
    config.query_timeout_ = std::chrono::seconds(2);
    std::optional<ruvia::db_error::code_type> failure;
    try {
        (void)ruvia::test::run_db_migration(config, migrations);
    } catch (const ruvia::db_error& error) {
        failure = error.code();
    }
    EXPECT_TRUE(failure == ruvia::db_error::code_type::io_error);
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// The migrator looks a DNS host up on its loop's blocking pool: a name that
// cannot resolve (RFC 6761 §6.4) fails the lookup, not the configuration.
TEST(Ruvia, db_migrator_looks_up_a_dns_host_on_its_loop_pool) {
    ruvia::db_config config{.driver_ = ruvia::db_driver::postgresql,
        .host_ = "ruvia-migrator.invalid",
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
    config.connect_timeout_ = std::chrono::seconds(1);
    bool lookup_failed = false;
    try {
        (void)ruvia::test::run_db_migration(config, std::span<const ruvia::db_migration>{});
    } catch (const ruvia::db_error&) {
        lookup_failed = true;
    }
    EXPECT_TRUE(lookup_failed);
}

TEST(Ruvia, db_migrator_applies_and_records_postgresql_migrations_on_the_callers_loop) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration(
        {.id_ = "001", .sql_ = "CREATE TABLE t(id INT)", .atomicity_ = ruvia::db_migration_atomicity::unwrapped})};
    peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>({std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"},
        {.column_ = "pg_advisory_lock", .rows_ = {""}},
        {.column_ = "", .command_ = "RESET"},
        {.column_ = "", .command_ = "CREATE TABLE"},
        {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}},
        {.column_ = "checksum"},
        {.column_ = "", .command_ = "CREATE TABLE"},
        {.column_ = "", .command_ = "INSERT 0 1"},
        {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}})}));
    const auto report = ruvia::test::run_db_migration(scripted_config(peer, ruvia::db_driver::postgresql), migrations);
    ASSERT_EQ(report.applied().size(), std::size_t{1});
    EXPECT_EQ(report.applied()[0], "001");
    EXPECT_TRUE(report.skipped().empty());
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A stop request or deadline ends the run while the backend still holds the
// advisory lock request: the dedicated session closes, which is what releases
// a lock it may hold, and the run reports the operation's own outcome.
TEST(Ruvia, db_migrator_postgresql_run_ends_on_cancellation_and_timeout) {
    using peer_type = ruvia::test::backend_startup_peer;
    for (const bool cancel : {true, false}) {
        SCOPED_TRACE(cancel ? "cancelled" : "timed out");
        peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>({std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"},
            {.column_ = "pg_advisory_lock", .rows_ = {""}, .hold_ = 1}})}));
        ruvia::stop_source stop;
        const auto lock_requested = [&] {
            for (const auto& parse : peer.postgres_parses()) {
                if (parse.sql_.find("pg_advisory_lock") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        const auto started = std::chrono::steady_clock::now();
        const auto code = cancel ? interrupted_migration(scripted_config(peer, ruvia::db_driver::postgresql), {.stop_token_ = stop.token()}, &stop, lock_requested)
                                 : interrupted_migration(scripted_config(peer, ruvia::db_driver::postgresql), {.timeout_ = std::chrono::milliseconds(200)}, nullptr, lock_requested);
        EXPECT_TRUE(code == (cancel ? ruvia::db_error::code_type::cancelled : ruvia::db_error::code_type::timeout));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
        // Answering the held lock only reaches a connection the run closed.
        peer.release_hold(1);
        peer.wait_for_disconnects();
    }
}

TEST(Ruvia, db_migrator_rejects_a_stopped_token_and_non_positive_timeout_before_connecting) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer;
    const auto config = scripted_config(peer, ruvia::db_driver::postgresql);
    ruvia::stop_source stop;
    stop.request_stop();
    std::optional<ruvia::db_error::code_type> code;
    try {
        (void)ruvia::test::run_db_migration(config, {}, {}, {.stop_token_ = stop.token()});
    } catch (const ruvia::db_error& error) {
        code = error.code();
    }
    EXPECT_TRUE(code == ruvia::db_error::code_type::cancelled);
    for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        EXPECT_THROW((void)ruvia::test::run_db_migration(config, {}, {}, {.timeout_ = invalid}), std::invalid_argument);
    }
    EXPECT_EQ(peer.startups(), std::size_t{0});
}

TEST(Ruvia, db_migrator_requires_a_valid_loop_and_runs_only_on_it) {
    EXPECT_THROW((void)ruvia::db_migrator(ruvia::event_loop{}, ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}),
        std::invalid_argument);
    ruvia::event_loop_pool without_lookups({.loop_count_ = 2});
    EXPECT_THROW((void)ruvia::db_migrator(without_lookups.loop(0),
                     ruvia::db_config{.driver_ = ruvia::db_driver::postgresql, .host_ = "db.example"}),
        std::invalid_argument);
    const ruvia::db_migrator migrator(without_lookups.loop(0),
        ruvia::db_config{.driver_ = ruvia::db_driver::postgresql, .host_ = "127.0.0.1", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}});
    auto migration = migrator.migrate({});
    without_lookups.start();
    auto run = without_lookups.loop(1).start(std::move(migration));
    run.wait();
    without_lookups.join();
    EXPECT_THROW((void)std::move(run).get(), std::logic_error);
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, db_migrator_rejects_unverifiable_mariadb_history) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE t(id INT)"})};
    for (const bool null : {false, true}) {
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("1")})},
                           {.column_ = std::string("")},
                           {.rows_ = std::vector<std::string>({std::string("1")})},
                           {.column_ = std::string("checksum"), .rows_ = std::vector<std::string>({std::string("")}), .null_rows_ = std::vector<bool>(1, null)},
                           {.column_ = std::string(""), .affected_rows_ = 1},
                           {.column_ = std::string("")}}),
            peer_type::after_startup::idle);
        check_unverifiable_history(peer, ruvia::db_driver::mariadb, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_mariadb_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("1")})},
                       {.column_ = std::string("")},
                       {},
                       {.column_ = std::string("")},
                       {.column_ = std::string("")}}),
        peer_type::after_startup::idle);
    check_unverifiable_history(peer, ruvia::db_driver::mariadb, {});
}

TEST(Ruvia, db_migrator_applies_and_records_mariadb_migrations_on_the_callers_loop) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE IF NOT EXISTS t(id INT)"})};
    peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = {"1"}},
                       {.column_ = ""},
                       {.rows_ = {"1"}},
                       {.column_ = "checksum"},
                       {.column_ = ""},
                       {.column_ = "", .affected_rows_ = 1},
                       {.column_ = ""}}),
        peer_type::after_startup::idle);
    const auto report = ruvia::test::run_db_migration(scripted_config(peer, ruvia::db_driver::mariadb), migrations);
    ASSERT_EQ(report.applied().size(), std::size_t{1});
    EXPECT_EQ(report.applied()[0], "001");
    EXPECT_TRUE(report.skipped().empty());
}

TEST(Ruvia, db_migrator_mariadb_run_ends_on_cancellation_and_timeout) {
    using peer_type = ruvia::test::backend_startup_peer;
    for (const bool cancel : {true, false}) {
        SCOPED_TRACE(cancel ? "cancelled" : "timed out");
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = {"1"}, .hold_ = 1}}),
            peer_type::after_startup::idle);
        ruvia::stop_source stop;
        const auto lock_requested = [&] {
            for (const auto& command : peer.maria_commands()) {
                if (command.find("GET_LOCK") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        const auto started = std::chrono::steady_clock::now();
        const auto code = cancel ? interrupted_migration(scripted_config(peer, ruvia::db_driver::mariadb), {.stop_token_ = stop.token()}, &stop, lock_requested)
                                 : interrupted_migration(scripted_config(peer, ruvia::db_driver::mariadb), {.timeout_ = std::chrono::milliseconds(200)}, nullptr, lock_requested);
        EXPECT_TRUE(code == (cancel ? ruvia::db_error::code_type::cancelled : ruvia::db_error::code_type::timeout));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
        // Answering the held lock only reaches a connection the run closed.
        peer.release_hold(1);
        peer.wait_for_disconnects();
    }
}
#endif
