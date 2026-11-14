#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include "backend_client_fixture.h"
#include "blocking_pool.h"
#include "db.h"
#include "db_query.h"
#include "db_session.h"
#include "event_loop.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "task_scope.h"
#include "worker_task_fixture.h"
namespace ruvia::test {

// Runs one migration to completion on a fresh loop, whose blocking pool looks
// a DNS host up, and returns its report or rethrows its failure.
[[nodiscard]] inline db_migration_report run_db_migration(const db_config& config, std::span<const db_migration> migrations, const db_migrator_options& options = {}, operation_options operation = {}) {
    blocking_pool host_lookups({.thread_count_ = 1});
    event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &host_lookups});
    const db_migrator migrator(loops.loop(0), config, options);
    auto migration = migrator.migrate(migrations, std::move(operation));
    loops.start();
    auto run = loops.loop(0).start(std::move(migration));
    run.wait();
    loops.join();
    return std::move(run).get();
}

}  // namespace ruvia::test

namespace {
constexpr std::array database_drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
    ruvia::db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_driver::mariadb,
#endif
};
}  // namespace

TEST(Ruvia, db_pool_rejects_zero_capacity_at_configuration) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    for (const auto driver : database_drivers) {
        const ruvia::db_config config{.driver_ = driver, .pool_size_per_worker_ = 0};
        bool client_rejected = false;
        bool migrator_rejected = false;
        try {
            ruvia::db_client client(pool.loop(0), config);
        } catch (const std::invalid_argument&) {
            client_rejected = true;
        }
        try {
            ruvia::db_migrator migrator(pool.loop(0), config);
        } catch (const std::invalid_argument&) {
            migrator_rejected = true;
        }
        EXPECT_TRUE(client_rejected);
        EXPECT_TRUE(migrator_rejected);
    }
    pool.join();
}

TEST(Ruvia, db_client_needs_a_loop_with_a_blocking_pool_to_look_up_a_dns_host) {
    for (const auto driver : database_drivers) {
        const ruvia::db_config config{.driver_ = driver, .host_ = "localhost", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
        bool rejected = false;
        {
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            try {
                ruvia::db_client client(pool.loop(0), config);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            pool.join();
        }
        EXPECT_TRUE(rejected);
        // The same configuration is accepted once the loop borrows a pool.
        ruvia::blocking_pool lookups({.thread_count_ = 1});
        ruvia::event_loop_pool pool({.loop_count_ = 1, .blocking_pool_ = &lookups});
        {
            ruvia::db_client client(pool.loop(0), config);
        }
        pool.join();
        lookups.join();
    }
}

TEST(Ruvia, db_pool_opens_the_configured_number_of_connections) {
    using peer_type = ruvia::test::backend_startup_peer;
    for (const auto driver : database_drivers) {
        peer_type peer(driver == ruvia::db_driver::postgresql ? peer_type::protocol::postgresql : peer_type::protocol::mariadb, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>(), 3);
        auto config = peer.config();
        config.driver_ = driver;
        config.pool_size_per_worker_ = 3;
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::db_client client(attachment.loop(), config);
        // Retained configuration owns its values independently of the caller.
        config.host_ = "invalid host";
        config.port_ = 0;
        config.pool_size_per_worker_ = 0;
        config.connect_timeout_ = std::chrono::milliseconds::zero();
        const auto connected = [](ruvia::db_client& value) -> ruvia::task<void> {
            auto operation = value.db().query("SELECT 1");
            (void)operation;
            co_return;
        };
        ruvia::test::drive_backend_client(attachment, client, connected);
        peer.wait_for_disconnects();
        EXPECT_EQ(peer.startups(), std::size_t{3});
    }
}

TEST(Ruvia, db_pool_failed_startup_retires_already_connected_slots) {
    using peer_type = ruvia::test::backend_startup_peer;
    for (const auto driver : database_drivers) {
        peer_type peer(driver == ruvia::db_driver::postgresql ? peer_type::protocol::postgresql : peer_type::protocol::mariadb, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>(), 2);
        auto config = peer.config();
        config.driver_ = driver;
        config.pool_size_per_worker_ = 3;
        config.connect_timeout_ = std::chrono::seconds(1);
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::db_client client(attachment.loop(), config);
        bool entered = false;
        bool timed_out = false;
        const auto connected = [&](ruvia::db_client&) -> ruvia::task<void> {
            entered = true;
            co_return;
        };
        try {
            ruvia::test::drive_backend_client(attachment, client, connected);
        } catch (const ruvia::db_error& error) {
            timed_out = error.code() == ruvia::db_error::code_type::timeout;
        }
        EXPECT_TRUE(timed_out);
        EXPECT_TRUE(!entered);
        peer.wait_for_disconnects();
        EXPECT_EQ(peer.startups(), std::size_t{2});
    }
}

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, db_pool_transaction_retirement_checks_server_state_before_reuse) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    // Successful control commands may chain a new transaction or leave
    // autocommit disabled. Failed statements also retire via ROLLBACK.
    for (const auto status : {std::uint16_t{2}, std::uint16_t{3}, std::uint16_t{0}}) {
        for (int ending = 0; ending < 3; ++ending) {
            std::vector<peer_type::maria_reply> replies({{.column_ = std::string(""), .status_ = 3}});
            if (ending == 2) {
                replies.push_back({.error_ = "missing table", .code_ = 1146, .sql_state_ = "42S02"});
            }
            replies.push_back({.column_ = "", .status_ = status});
            replies.push_back({.rows_ = {"available"}});
            peer_type peer(std::move(replies), peer_type::after_startup::idle, status == 2 ? 1 : 2);
            auto config = peer.config();
            config.driver_ = db_driver::mariadb;
            config.pool_size_per_worker_ = 1;
            auto& io = test::new_test_io_context();
            auto attachment = attach_event_loop(io);
            db_client client(attachment.loop(), config);
            test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
                const auto handle = connected.db().with_options({.timeout_ = std::chrono::seconds(5)});
                auto transaction = co_await handle.begin_transaction();
                if (ending == 0) {
                    co_await transaction.commit();
                } else if (ending == 1) {
                    co_await transaction.rollback();
                } else {
                    bool failed = false;
                    try {
                        (void)co_await transaction.query("SELECT * FROM missing_table");
                    } catch (const db_error& error) {
                        failed = error.code() == db_error::code_type::statement_failed && error.sql_state() == "42S02";
                    }
                    EXPECT_TRUE(failed);
                }
                EXPECT_TRUE(!transaction.active());
                const auto rows = co_await handle.query("SELECT v FROM records");
                EXPECT_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    EXPECT_TRUE(rows.front()["v"].as<std::string_view>() == "available");
                }
            });
            EXPECT_EQ(peer.startups(), status == 2 ? std::size_t{1} : std::size_t{2});
        }
    }
}
#endif

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_pool_allows_parallel_transactions_and_reuses_released_connections) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}, {.rows_ = std::vector<std::string>({std::string("available")})}, {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}}), 2);
    auto config = peer.config();
    config.pool_size_per_worker_ = 2;
    config.acquire_timeout_ = std::chrono::milliseconds(100);
    auto& io = test::new_test_io_context();
    auto attachment = attach_event_loop(io);
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& value) -> task<void> {
        auto handle = value.db();
        auto first = co_await handle.begin_transaction();
        auto second = co_await handle.begin_transaction();
        bool timed_out = false;
        try {
            (void)co_await handle.query("SELECT exhausted");
        } catch (const db_error& error) {
            timed_out = error.code() == db_error::code_type::timeout;
        }
        EXPECT_TRUE(timed_out);

        stop_source stop;
        task_scope pending(value.worker());
        bool cancelled = false;
        const auto wait_for_connection = [&]() -> task<void> {
            try {
                (void)co_await handle.with_options({.stop_token_ = stop.token()}).query("SELECT cancelled");
            } catch (const db_error& error) {
                cancelled = error.code() == db_error::code_type::cancelled;
            }
        };
        pending.spawn(wait_for_connection());
        stop.request_stop();
        co_await pending.join();
        EXPECT_TRUE(cancelled);

        co_await first.rollback();
        const auto result = co_await handle.query("SELECT available");
        EXPECT_EQ(result.size(), std::size_t{1});
        co_await second.rollback();
    });
    peer.wait_for_disconnects();
    EXPECT_EQ(peer.startups(), std::size_t{2});
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        EXPECT_EQ(statements[3].sql_, "SELECT available");
    }
}

TEST(Ruvia, db_pool_shutdown_wakes_waiters_while_all_connections_are_leased) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}}), 2);
    auto config = peer.config();
    config.pool_size_per_worker_ = 2;
    auto& io = test::new_test_io_context();
    auto attachment = attach_event_loop(io);
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& value) -> task<void> {
        auto handle = value.db();
        auto first = co_await handle.begin_transaction();
        auto second = co_await handle.begin_transaction();
        task_scope pending(value.worker());
        bool closing = false;
        const auto wait_for_connection = [&]() -> task<void> {
            try {
                (void)co_await handle.query("SELECT blocked");
            } catch (const db_error& error) {
                closing = error.code() == db_error::code_type::closing;
            }
        };
        pending.spawn(wait_for_connection());
        value.close();
        co_await pending.join();
        EXPECT_TRUE(closing);
    });
    peer.wait_for_disconnects();
}
#endif

namespace {

using ruvia::test::throws_on;

[[nodiscard]] ruvia::db_config test_db_config() {
#ifdef RUVIA_ENABLE_MARIADB
    return ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    return ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
}

// A loop a migrator can bind; its blocking pool admits a DNS host.
struct migrator_loop final {
    ruvia::blocking_pool host_lookups_{{.thread_count_ = 1}};
    ruvia::event_loop_pool loops_{{.loop_count_ = 1, .blocking_pool_ = &host_lookups_}};

    [[nodiscard]] ruvia::event_loop loop() const {
        return loops_.loop(0);
    }
};

}  // namespace

TEST(Ruvia, scoped_operation_scope_tracks_cold_owner_operations) {
    ruvia::operation_scope operation_scope;
    auto cold_task = []() -> ruvia::task<void> {
        co_return;
    }();
    {
        auto operation = ruvia::make_scoped_operation(operation_scope, std::move(cold_task));
        EXPECT_TRUE(operation_scope.has_pending_operations());
    }
    EXPECT_TRUE(!operation_scope.has_pending_operations());
}

TEST(Ruvia, db_error_carries_category_and_native_diagnostics) {
    const ruvia::db_error timeout(ruvia::db_error::code_type::timeout, ruvia::db_driver::mariadb, "timeout", 1205, "HY000");
    const ruvia::db_error unique_violation(ruvia::db_error::code_type::statement_failed, ruvia::db_driver::postgresql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::db_error cancelled(ruvia::db_error::code_type::cancelled, ruvia::db_driver::postgresql, "cancelled");
    const ruvia::db_error closing(ruvia::db_error::code_type::closing, ruvia::db_driver::mariadb, "closing");
    EXPECT_TRUE(timeout.code() == ruvia::db_error::code_type::timeout);
    EXPECT_TRUE(timeout.driver() == ruvia::db_driver::mariadb);
    EXPECT_TRUE(timeout.native_code() == 1205);
    EXPECT_TRUE(timeout.sql_state() == "HY000");
    EXPECT_TRUE(unique_violation.driver() == ruvia::db_driver::postgresql);
    EXPECT_TRUE(unique_violation.sql_state() == "23505");
    EXPECT_TRUE(unique_violation.constraint_name() == "uq_jobs_key");
    EXPECT_TRUE(!unique_violation.native_code());
    EXPECT_TRUE(cancelled.code() == ruvia::db_error::code_type::cancelled);
    EXPECT_TRUE(!cancelled.native_code());
    EXPECT_TRUE(!cancelled.sql_state());
    EXPECT_TRUE(!cancelled.constraint_name());
    EXPECT_TRUE(closing.code() == ruvia::db_error::code_type::closing);
}

TEST(Ruvia, db_config_rejects_non_positive_timeouts) {
    const migrator_loop loop;
    auto config = test_db_config();
    for (const auto field : {&ruvia::db_config::connect_timeout_, &ruvia::db_config::read_timeout_, &ruvia::db_config::write_timeout_, &ruvia::db_config::query_timeout_, &ruvia::db_config::acquire_timeout_, &ruvia::db_config::transaction_timeout_}) {
        for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
            config.*field = invalid;
            EXPECT_TRUE(throws_on([&] {
                ruvia::db_migrator migrator(loop.loop(), config);
            }));
        }
        (config.*field).reset();
    }
}

TEST(Ruvia, db_migrator_rejects_non_positive_lock_timeouts) {
    const migrator_loop loop;
    ruvia::db_migrator_options options;
    for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        options.lock_timeout_ = invalid;
        EXPECT_TRUE(throws_on([&] {
            ruvia::db_migrator migrator(loop.loop(), test_db_config(), options);
        }));
    }
}

TEST(Ruvia, db_migrator_validates_before_opening_connection) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    const migrator_loop loop;
    const ruvia::db_migrator migrator(loop.loop(), test_db_config());
    bool rejected = false;
    try {
        (void)migrator.migrate(std::span<const ruvia::db_migration>(migrations));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "database migration ids must be unique, including case";
    }
    EXPECT_TRUE(rejected);
}

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, db_migrator_treats_mariadb_double_dash_at_end_of_sql_as_comment) {
    // The cold run is never started: only validation can reject the list.
    const migrator_loop loop;
    const auto validation_error = [&](std::string_view sql) {
        const std::array<ruvia::db_migration, 1> migrations{{
            ruvia::db_migration{{.id_ = "001", .sql_ = std::string(sql)}},
        }};
        auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb, .port_ = std::uint16_t{1}};
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        const ruvia::db_migrator migrator(loop.loop(), config);
        try {
            (void)migrator.migrate(std::span<const ruvia::db_migration>(migrations));
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    EXPECT_EQ(validation_error("CREATE TABLE t (a INT);--"), std::string());
    EXPECT_EQ(validation_error("--"), std::string("database migration SQL must not be empty"));
    EXPECT_EQ(validation_error("SELECT 1;--x"), std::string("database migration must contain exactly one SQL statement"));
}
#endif

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_migrator_rejects_unrepresentable_postgresql_lock_timeout_before_connecting) {
    const migrator_loop loop;
    const auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
    for (const auto timeout : {ruvia::timeout_duration(std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()}), ruvia::timeout_duration(std::chrono::milliseconds{std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1})}) {
        const ruvia::db_migrator_options options{.lock_timeout_ = timeout};
        EXPECT_THROW((void)ruvia::db_migrator(loop.loop(), config, options), std::invalid_argument);
    }
}
#endif

TEST(Ruvia, db_migrator_copies_public_configuration) {
    const migrator_loop loop;
    std::optional<ruvia::db_migrator> migrator;
    {
        auto config = test_db_config();
        config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.username_ = std::string(80, 'u');
        config.password_ = std::string(80, 'p');
        config.database_ = std::string(80, 'd');
        const std::string table_name(60, 't');
        ruvia::db_migrator_options options{
            .table_ = table_name,
            .lock_timeout_ = std::chrono::seconds(30),
        };
        migrator.emplace(loop.loop(), config, options);
    }
    // Opaque owner + four long DB strings + the long migration table all use
    // the caller's resource rather than the input strings' allocators.
    migrator.reset();
}

TEST(Ruvia, db_migrator_validates_complete_configuration_before_allocating) {
    const migrator_loop loop;
    {
        auto config = test_db_config();
        config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.connect_timeout_ = std::chrono::milliseconds::zero();
        const std::string table_name(60, 't');
        const ruvia::db_migrator_options options{
            .table_ = table_name,
        };

        EXPECT_TRUE(throws_on([&] {
            (void)ruvia::db_migrator(loop.loop(), config, options);
        }));
    }
    {
        auto config = test_db_config();
        config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        const ruvia::db_migrator_options options{
            .table_ = "invalid-table-name",
        };

        EXPECT_TRUE(throws_on([&] {
            (void)ruvia::db_migrator(loop.loop(), config, options);
        }));
    }
}

TEST(Ruvia, db_migrator_rejects_nul_bytes_in_credentials) {
    using namespace std::string_literals;
    const migrator_loop loop;
    for (const auto field : {&ruvia::db_config::username_, &ruvia::db_config::password_, &ruvia::db_config::database_}) {
        auto config = test_db_config();
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.*field = "app\0suffix"s;
        EXPECT_TRUE(throws_on([&] {
            (void)ruvia::db_migrator(loop.loop(), config, {});
        }));
    }
}

TEST(Ruvia, database_config_rejects_non_host_dns_inputs) {
    const migrator_loop loop;
    for (const std::string_view host : {std::string_view("bad_name"), std::string_view("1.2.3.999"), std::string_view("host..example"), std::string_view("-host.example")}) {
        auto config = test_db_config();
        config.host_ = host;
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        EXPECT_TRUE(throws_on([&] {
            (void)ruvia::db_migrator(loop.loop(), config, {});
        }));
    }
}

TEST(Ruvia, db_migrator_validates_migration_list_before_allocating_runtime) {
    const std::array<ruvia::db_migration, 2> migrations{{
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 1"}},
        ruvia::db_migration{{.id_ = "duplicate", .sql_ = "SELECT 2"}},
    }};
    auto config = test_db_config();
    config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    const std::string table_name(60, 't');
    const ruvia::db_migrator_options options{
        .table_ = table_name,
    };
    const migrator_loop loop;
    const ruvia::db_migrator migrator(loop.loop(), config, options);

    EXPECT_TRUE(throws_on([&] {
        (void)migrator.migrate(std::span<const ruvia::db_migration>(migrations));
    }));
}

TEST(Ruvia, db_expression_values_and_db_value_copies_own_parameter_text) {
    const std::string value(128, 'v');
    ruvia::db_query source_value;
    source_value.select(source_value.value(value));
    const auto statement = source_value.compile(ruvia::db_driver::postgresql);
    ASSERT_EQ(statement.params().size(), std::size_t{1});

    for (const bool through_expressions : {false, true}) {
        ruvia::db_query destination;
        ruvia::db_query expressions;
        destination.select(through_expressions ? destination.import_expression(expressions.value(statement.params()[0])) : destination.value(statement.params()[0]));
        const auto compiled = destination.compile(ruvia::db_driver::postgresql);
        ASSERT_EQ(compiled.params().size(), std::size_t{1});
        EXPECT_EQ(compiled.params()[0].text(), value);
    }

    std::optional<ruvia::db_value> copied;
    {
        const std::string text(4096, 'v');
        ruvia::db_query query;
        query.select(query.value(text));
        const auto compiled = query.compile(ruvia::db_driver::postgresql);
        ASSERT_EQ(compiled.params().size(), std::size_t{1});
        copied.emplace(compiled.params()[0]);
    }
    const ruvia::db_value second(*copied);
    EXPECT_EQ(second.text(), std::string(4096, 'v'));
}

namespace {

enum class statement_kind { query,
    execute,
    stream };

ruvia::task<void> run_statement(ruvia::db_session& handle, statement_kind kind) {
    switch (kind) {
        case statement_kind::query:
            (void)co_await handle.query("SELECT v");
            break;
        case statement_kind::execute:
            (void)co_await handle.execute("SELECT v");
            break;
        case statement_kind::stream: {
            auto stream = co_await handle.query_stream("SELECT v");
            while (co_await stream.read()) {
            }
            break;
        }
    }
}

// `peer` answers "before" and then ends the session while the pool holds it
// idle; it answers "after" on the next connection.
void check_session_ended_while_idle(ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver) {
    using namespace ruvia;
    auto config = peer.config();
    config.driver_ = driver;
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config);
    std::string before{};
    std::optional<db_error::code_type> code;
    std::string after{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db().with_options({.timeout_ = std::chrono::seconds(5)});
        const auto first = co_await handle.query("SELECT v FROM before");
        before = first.front()["v"].as<std::string_view>().value_or("");
        peer.wait_for_idle_close();
        // The peer has closed its end; give its FIN the loopback hop to this
        // worker's socket before the next lease looks at it.
        (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(50));
        try {
            const auto second = co_await handle.query("SELECT v FROM after");
            after = second.front()["v"].as<std::string_view>().value_or("");
        } catch (const db_error& error) {
            code = error.code();
        }
    });
    EXPECT_TRUE(before == "before");
    EXPECT_TRUE(!code.has_value());
    EXPECT_TRUE(after == "after");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

}  // namespace

// The peer drops the connection after the first row of a row set. Every
// statement shape must report that as an I/O failure: neither as caller misuse
// ("requires row-producing SQL"), nor -- for execute() -- as success.
TEST(Ruvia, db_connection_lost_mid_result_is_an_io_error) {
    using namespace ruvia;
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        for (const auto kind : {statement_kind::query, statement_kind::execute, statement_kind::stream}) {
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            std::optional<db_error::code_type> code;
            test::with_connected_db_client(attachment, db_config{.driver_ = driver}, test::backend_startup_peer::after_startup::drop_mid_result, [&](db_client& client) -> task<void> {
                auto handle = client.db();
                try {
                    co_await run_statement(handle, kind);
                } catch (const db_error& error) {
                    code = error.code();
                }
            });
            EXPECT_TRUE(code == db_error::code_type::io_error);
        }
    }
}

// The server ends a pooled session while the pool holds it idle (PostgreSQL
// idle_session_timeout or pg_terminate_backend, MariaDB wait_timeout or KILL).
// The next lease looks at the connection before sending anything, reconnects,
// and the statement succeeds on the new session.
TEST(Ruvia, db_session_ended_while_idle_reconnects_before_the_next_statement) {
    using peer_type = ruvia::test::backend_startup_peer;
#ifdef RUVIA_ENABLE_POSTGRESQL
    {
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("before")}), .close_after_ = true}, {.rows_ = std::vector<std::string>({std::string("after")})}}));
        check_session_ended_while_idle(peer, ruvia::db_driver::postgresql);
    }
#endif
#ifdef RUVIA_ENABLE_MARIADB
    {
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("before")}), .close_after_ = true}, {.rows_ = std::vector<std::string>({std::string("after")})}}), peer_type::after_startup::reaccept);
        check_session_ended_while_idle(peer, ruvia::db_driver::mariadb);
    }
#endif
}

#ifdef RUVIA_ENABLE_POSTGRESQL
// A statement the server rejected leaves the connection in sync. The peer
// accepts one connection, so the next statement succeeding proves the pool
// kept it rather than reconnecting.
TEST(Ruvia, db_rejected_statement_keeps_the_pooled_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.error_ = std::string("duplicate key value violates unique constraint"), .sql_state_ = std::string("23505")}, {.rows_ = std::vector<std::string>({std::string("kept")})}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::string sql_state{};
    std::string kept{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        try {
            (void)co_await handle.execute("INSERT INTO jobs (key) VALUES ('a')");
        } catch (const db_error& error) {
            code = error.code();
            sql_state = error.sql_state().value_or("");
        }
        const auto rows = co_await handle.query("SELECT v FROM records");
        kept = rows.front()["v"].as<std::string_view>().value_or("");
    });
    EXPECT_TRUE(code == db_error::code_type::statement_failed);
    EXPECT_TRUE(sql_state == "23505");
    EXPECT_TRUE(kept == "kept");
    EXPECT_EQ(peer.postgres_parses().size(), std::size_t{2});
}

// The server's text for a rejected statement can quote the row it refused;
// the error names it by SQLSTATE (and errno) without that text.
TEST(Ruvia, db_postgresql_statement_error_omits_server_text) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.error_ = std::string("duplicate key value violates unique constraint for secret@example.com"), .sql_state_ = std::string("23505")}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::string sql_state{};
    std::string message{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        try {
            (void)co_await handle.execute("INSERT INTO users (email) VALUES ($1)", std::string("secret@example.com"));
        } catch (const db_error& error) {
            code = error.code();
            sql_state = error.sql_state().value_or("");
            message = error.what();
        }
    });
    EXPECT_TRUE(code == db_error::code_type::statement_failed);
    EXPECT_TRUE(sql_state == "23505");
    EXPECT_TRUE(message.find("[sqlstate=23505]") != std::string::npos);
    EXPECT_TRUE(message.find("secret@example.com") == std::string::npos);
    EXPECT_TRUE(message.find("duplicate key") == std::string::npos);
}

// A statement that fails inside a transaction fails the transaction, and the
// pool rolls it back on the kept connection before the next caller gets it.
TEST(Ruvia, db_rejected_transaction_statement_rolls_back_on_the_kept_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.status_ = 'E', .error_ = std::string("syntax error at or near \"SELEC\"")}, {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}, {.rows_ = std::vector<std::string>({std::string("after")})}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    bool transaction_failed = false;
    std::string after{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto transaction = co_await handle.begin_transaction();
        try {
            (void)co_await transaction.query("SELEC v FROM records");
        } catch (const db_error& error) {
            code = error.code();
        }
        transaction_failed = !transaction.active();
        const auto rows = co_await handle.query("SELECT v FROM records");
        after = rows.front()["v"].as<std::string_view>().value_or("");
    });
    EXPECT_TRUE(code == db_error::code_type::statement_failed);
    EXPECT_TRUE(transaction_failed);
    EXPECT_TRUE(after == "after");
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{4});
    if (statements.size() == 4) {
        EXPECT_TRUE(statements[2].sql_ == "ROLLBACK");
    }
}

// A COPY result leaves the connection in COPY state with no ReadyForQuery to
// drain to, so the statement fails at once and the pool closes the slot: the
// next statement runs on a fresh connection.
TEST(Ruvia, db_copy_result_fails_the_statement_and_closes_the_connection) {
    using namespace ruvia;
    using direction = test::backend_startup_peer::postgres_copy_direction;
    for (const auto copy : {direction::input, direction::output}) {
        for (const auto kind : {statement_kind::query, statement_kind::execute}) {
            test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::reaccept, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.copy_ = copy}, {.rows_ = std::vector<std::string>({std::string("fresh")})}}));
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            db_client client(attachment.loop(), peer.config());
            std::optional<db_error::code_type> code;
            std::string fresh{};
            test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
                auto handle = connected.db();
                const auto sql = copy == direction::input ? "COPY records FROM STDIN" : "COPY records TO STDOUT";
                try {
                    if (kind == statement_kind::query) {
                        (void)co_await handle.query(sql);
                    } else {
                        (void)co_await handle.execute(sql);
                    }
                } catch (const db_error& error) {
                    code = error.code();
                }
                const auto rows = co_await handle.query("SELECT v FROM records");
                fresh = rows.front()["v"].as<std::string_view>().value_or("");
            });
            EXPECT_TRUE(code == db_error::code_type::protocol_error);
            EXPECT_TRUE(fresh == "fresh");
            EXPECT_EQ(peer.startups(), std::size_t{2});
            EXPECT_EQ(peer.postgres_parses().size(), std::size_t{2});
        }
    }
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
// The text and the prepared protocol both leave out the server's text for a
// rejected statement, which can quote the row it refused.
TEST(Ruvia, db_mariadb_statement_error_omits_server_text) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    const peer_type::maria_reply duplicate{.column_ = std::string(""), .error_ = std::string("Duplicate entry 'secret@example.com' for key 'email'"), .code_ = 1062, .sql_state_ = std::string("23000")};
    peer_type peer(std::vector<peer_type::maria_reply>({duplicate, duplicate}), peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.driver_ = db_driver::mariadb;
    db_client client(attachment.loop(), config);
    std::vector<std::string> messages;
    std::vector<std::optional<std::int64_t>> native_codes;
    std::vector<std::string> sql_states;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        const auto record = [&](const db_error& error) {
            messages.emplace_back(error.what());
            native_codes.push_back(error.native_code());
            sql_states.emplace_back(error.sql_state().value_or(""));
        };
        try {
            (void)co_await handle.execute("INSERT INTO users (email) VALUES ('secret@example.com')");
        } catch (const db_error& error) {
            record(error);
        }
        try {
            (void)co_await handle.execute("INSERT INTO users (email) VALUES (?)", std::string("secret@example.com"));
        } catch (const db_error& error) {
            record(error);
        }
    });
    ASSERT_EQ(messages.size(), std::size_t{2});
    for (std::size_t i = 0; i < messages.size(); ++i) {
        EXPECT_TRUE(native_codes[i] == std::optional<std::int64_t>(1062));
        EXPECT_TRUE(sql_states[i] == "23000");
        EXPECT_TRUE(messages[i].find("[errno=1062] [sqlstate=23000]") != std::string::npos);
        EXPECT_TRUE(messages[i].find("secret@example.com") == std::string::npos);
        EXPECT_TRUE(messages[i].find("Duplicate entry") == std::string::npos);
    }
}
#endif

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {

constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id = $1";

template <typename param_type>
ruvia::task<std::string> first_value(ruvia::db_session& handle, param_type param) {
    const ruvia::db_rows rows = co_await handle.query(cached_sql, param);
    co_return std::string(rows.front()["v"].as<std::string_view>().value_or(""));
}

using parameter_values = std::vector<std::optional<std::string>>;

}  // namespace

// A statement runs unnamed on its first use, is parsed as a named statement on
// its second, and is bound by name afterwards. The server fixes parameter
// types at Parse, so the same SQL with a differently typed parameter is a
// separate statement.
TEST(Ruvia, db_postgresql_repeated_statement_is_parsed_once_per_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})}, {.rows_ = std::vector<std::string>({std::string("c")})}, {.rows_ = std::vector<std::string>({std::string("d")})}, {.rows_ = std::vector<std::string>({std::string("e")})}, {.rows_ = std::vector<std::string>({std::string("f")})}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::vector<std::string> values{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        for (const std::int64_t id : {1, 2, 3}) {
            values.push_back(co_await first_value(handle, id));
        }
        for (const double id : {1.5, 2.5, 3.5}) {
            values.push_back(co_await first_value(handle, id));
        }
    });
    EXPECT_TRUE((values == std::vector<std::string>({std::string("a"), std::string("b"), std::string("c"), std::string("d"), std::string("e"), std::string("f")})));
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{4});
    if (statements.size() == 4) {
        for (const auto& statement : statements) {
            EXPECT_TRUE(statement.sql_ == cached_sql);
        }
        EXPECT_TRUE((statements[0].parameter_types_ == std::vector<std::uint32_t>(1, 0)));
        EXPECT_TRUE((statements[0].parameter_values_ == parameter_values({std::string("1")})));
        EXPECT_TRUE((statements[1].parameter_types_ == std::vector<std::uint32_t>(1, 0)));
        EXPECT_TRUE((statements[1].parameter_values_ == parameter_values({std::string("2"), std::string("3")})));
        EXPECT_TRUE((statements[2].parameter_types_ == std::vector<std::uint32_t>({701})));
        EXPECT_TRUE((statements[3].parameter_types_ == std::vector<std::uint32_t>({701})));
        EXPECT_EQ(statements[3].parameter_values_.size(), std::size_t{2});
    }
}

// Outside a transaction block, a cached statement the server lost (DEALLOCATE,
// DISCARD) or whose cached plan changed result type fails before it runs, so
// it runs once more without the caller seeing the failure.
TEST(Ruvia, db_postgresql_stale_cached_statement_is_retried) {
    using namespace ruvia;
    struct stale_case final {
        std::string_view sql_state_;
        std::string_view routine_;
    };
    for (const auto stale : {stale_case{"0A000", "RevalidateCachedQuery"}, stale_case{"26000", "FetchPreparedStatement"}}) {
        test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})}, {.error_ = std::string("stale statement"), .sql_state_ = std::string(stale.sql_state_), .routine_ = std::string(stale.routine_)}, {.rows_ = std::vector<std::string>({std::string("c")})}}));
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
        std::vector<std::string> values{};
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            auto handle = connected.db();
            for (int use = 0; use < 3; ++use) {
                values.push_back(co_await first_value(handle, std::int64_t{1}));
            }
        });
        EXPECT_TRUE((values == std::vector<std::string>({std::string("a"), std::string("b"), std::string("c")})));
        // Unnamed, named, and the retry parsing the statement again.
        EXPECT_EQ(peer.postgres_parses().size(), std::size_t{3});
        EXPECT_EQ(peer.startups(), std::size_t{1});
    }
}

// Inside a transaction block the failure has aborted the transaction, so it
// reaches the caller; the stale statement is parsed again on its next use.
TEST(Ruvia, db_postgresql_stale_statement_in_transaction_fails_and_is_parsed_again) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.rows_ = std::vector<std::string>({std::string("a")}), .status_ = 'T'}, {.rows_ = std::vector<std::string>({std::string("b")}), .status_ = 'T'}, {.status_ = 'E', .error_ = std::string("cached plan must not change result type"), .sql_state_ = std::string("0A000"), .routine_ = std::string("RevalidateCachedQuery")}, {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}, {.rows_ = std::vector<std::string>({std::string("c")})}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::string sql_state{};
    bool transaction_failed = false;
    std::string after{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto transaction = co_await handle.begin_transaction();
        for (int use = 0; use < 2; ++use) {
            (void)co_await transaction.query(cached_sql, std::int64_t{1});
        }
        try {
            (void)co_await transaction.query(cached_sql, std::int64_t{1});
        } catch (const db_error& error) {
            sql_state = error.sql_state().value_or("");
        }
        transaction_failed = !transaction.active();
        after = co_await first_value(handle, std::int64_t{1});
    });
    EXPECT_TRUE(sql_state == "0A000");
    EXPECT_TRUE(transaction_failed);
    EXPECT_TRUE(after == "c");
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        EXPECT_TRUE(statements[3].sql_ == "ROLLBACK");
        EXPECT_TRUE(statements[4].sql_ == cached_sql);
    }
}

// The same SQLSTATE raised while the statement runs is the statement's own
// failure: it is reported, not retried, and the statement is parsed again on
// its next use.
TEST(Ruvia, db_postgresql_stale_sql_state_raised_by_the_statement_is_not_retried) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false, test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})}, {.error_ = std::string("not supported"), .sql_state_ = std::string("0A000"), .routine_ = std::string("ExecInitNode")}, {.rows_ = std::vector<std::string>({std::string("c")})}}));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::string after{};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        for (int use = 0; use < 2; ++use) {
            (void)co_await first_value(handle, std::int64_t{1});
        }
        try {
            (void)co_await first_value(handle, std::int64_t{1});
        } catch (const db_error& error) {
            code = error.code();
        }
        after = co_await first_value(handle, std::int64_t{1});
    });
    EXPECT_TRUE(code == db_error::code_type::statement_failed);
    EXPECT_TRUE(after == "c");
    EXPECT_EQ(peer.postgres_parses().size(), std::size_t{3});
}
#endif

namespace database_tls_tests {

class handshake_peer final {
public:
    handshake_peer(asio::ssl::context& tls, bool maria) :
        acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
        stream_(io_, tls),
        deadline_(io_, std::chrono::seconds(5)),
        maria_(maria),
        port_(acceptor_.local_endpoint().port()) {
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                close();
            }
        });
        done_ = asio::co_spawn(io_, serve(), asio::use_future);
        thread_ = std::thread([this] {
            io_.run();
        });
    }
    ~handshake_peer() {
        asio::post(io_, [this] {
            close();
        });
        thread_.join();
    }
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
    [[nodiscard]] bool received_application_data() {
        done_.get();
        return received_;
    }

private:
    void close() noexcept {
        std::error_code ignored;
        acceptor_.close(ignored);
        stream_.next_layer().close(ignored);
        deadline_.cancel();
    }
    asio::awaitable<void> serve() {
        struct cleanup final {
            handshake_peer& peer_;
            ~cleanup() {
                peer_.close();
            }
        } guard{*this};
        co_await acceptor_.async_accept(stream_.next_layer(), asio::use_awaitable);
        if (maria_) {
            std::string greeting(1, '\x0a');
            greeting.append("8.0.0-test", 11);
            greeting.append("\x01\0\0\0", 4);
            greeting.append("12345678\0", 9);
            greeting.append("\x05\xaa", 2);
            greeting.push_back('\x2d');
            greeting.append("\x02\0", 2);
            greeting.push_back('\x0a');
            greeting.push_back('\0');
            greeting.push_back('\x15');
            greeting.append(10, '\0');
            greeting.append("abcdefghijkl\0", 13);
            greeting.append("mysql_native_password", 22);
            std::string packet(1, static_cast<char>(greeting.size()));
            packet.append(3, '\0');
            packet.append(greeting);
            co_await asio::async_write(stream_.next_layer(), asio::buffer(packet), asio::use_awaitable);
            std::array<unsigned char, 4> header{};
            co_await asio::async_read(stream_.next_layer(), asio::buffer(header), asio::use_awaitable);
            const auto size = std::size_t{header[0]} | (std::size_t{header[1]} << 8) | (std::size_t{header[2]} << 16);
            if (size != 32) {
                throw std::runtime_error("expected MariaDB SSLRequest");
            }
            std::vector<unsigned char> request(size);
            co_await asio::async_read(stream_.next_layer(), asio::buffer(request), asio::use_awaitable);
        } else {
            std::array<unsigned char, 8> request{};
            co_await asio::async_read(stream_.next_layer(), asio::buffer(request), asio::use_awaitable);
            constexpr std::array<unsigned char, 8> expected{0, 0, 0, 8, 4, 210, 22, 47};
            if (request != expected) {
                throw std::runtime_error("expected PostgreSQL SSLRequest");
            }
            co_await asio::async_write(stream_.next_layer(), asio::buffer("S", 1), asio::use_awaitable);
        }
        std::error_code error;
        co_await stream_.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        std::array<char, 4> length{};
        const auto size = co_await asio::async_read(stream_, asio::buffer(length), asio::redirect_error(asio::use_awaitable, error));
        received_ = !error && size == length.size();
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    asio::steady_timer deadline_;
    bool maria_;
    std::uint16_t port_;
    bool received_{false};
    std::future<void> done_;
    std::thread thread_;
};

bool sends_startup(ruvia::db_driver driver, const char* reference, const char* common_name, const char* san, std::uint64_t record_limit = std::uint64_t{1} << 23, int version = 0, bool elliptic_curve = false, bool mutual_tls = false, const char* cipher = nullptr, ruvia::tls_version minimum = ruvia::tls_version::tls_1_2) {
    ruvia::test::tls_identity identity(common_name, san, elliptic_curve);
    auto* const context = identity.context_.native_handle();
    if (version != 0) {
        // Only the adversarial test peer relaxes OpenSSL's legacy-signature
        // policy. The database client must retain the production policy.
        SSL_CTX_set_security_level(context, 0);
        if (SSL_CTX_set_min_proto_version(context, version) != 1 || SSL_CTX_set_max_proto_version(context, version) != 1) {
            throw std::runtime_error("cannot pin database test peer TLS version");
        }
    }
    if (cipher != nullptr && SSL_CTX_set_cipher_list(context, cipher) != 1) {
        throw std::runtime_error("cannot pin database test peer cipher");
    }
    if (mutual_tls) {
        if (SSL_CTX_load_verify_locations(context, identity.ca_file_.string().c_str(), nullptr) != 1) {
            throw std::runtime_error("cannot trust database test client identity");
        }
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
    handshake_peer peer(identity.context_, driver == ruvia::db_driver::mariadb);
    ruvia::db_config config{.driver_ = driver, .host_ = "127.0.0.1", .port_ = peer.port(), .username_ = "test", .database_ = "test"};
    config.tls_.ca_file_ = identity.ca_file_;
    config.tls_.minimum_version_ = minimum;
    if (mutual_tls) {
        // libpq rejects client private keys readable by group/other users;
        // the generic server identity fixture does not restrict its key file.
        std::filesystem::permissions(identity.private_key_file(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        config.tls_.identity_ = ruvia::tls_identity_config{.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
    }
    if (driver == ruvia::db_driver::postgresql) {
        config.tls_.server_name_ = reference;
    }
    config.tls_.max_outgoing_records_ = record_limit;
    config.connect_timeout_ = std::chrono::seconds(3);
    bool connect_failed = false;
    try {
        (void)ruvia::test::run_db_migration(config, {});
    } catch (const ruvia::db_error& error) {
        connect_failed = error.code() == ruvia::db_error::code_type::connect_failed;
    }
    if (!connect_failed) {
        throw std::runtime_error("test peer must end the database startup");
    }
    return peer.received_application_data();
}

void checks_protocol_floor(ruvia::db_driver driver) {
    constexpr auto limit = std::uint64_t{1} << 23;
    for (const bool elliptic_curve : {false, true}) {
        SCOPED_TRACE(elliptic_curve ? "ECDSA" : "RSA");
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", limit, TLS1_2_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "127.0.0.1", "", limit, TLS1_2_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2", limit, TLS1_2_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", 1, TLS1_2_VERSION, elliptic_curve));
        // RFC 8996 §2-3: TLS 1.0 and 1.1 are never negotiated.
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", limit, TLS1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", limit, TLS1_2_VERSION, elliptic_curve, true));
        const auto* const cbc = elliptic_curve ? "ECDHE-ECDSA-AES128-SHA" : "ECDHE-RSA-AES128-SHA";
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", limit, TLS1_2_VERSION, elliptic_curve, false, cbc));
    }
}

void checks_configured_protocol_floor(ruvia::db_driver driver) {
    for (const auto minimum : {ruvia::tls_version::tls_1_2, ruvia::tls_version::tls_1_3}) {
        for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(static_cast<int>(minimum));
            SCOPED_TRACE(version);
            EXPECT_EQ(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", std::uint64_t{1} << 23, version, false, false, nullptr, minimum), version >= static_cast<int>(minimum));
        }
    }
}

}  // namespace database_tls_tests

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, postgresql_tls_checks_san_identity_before_sending_startup) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::postgresql;
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "audit.test", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "audit.test", "DNS:other.test"));
    EXPECT_TRUE(sends_startup(driver, "audit.test", "other.test", "DNS:audit.test"));
    EXPECT_TRUE(sends_startup(driver, "AUDIT.test.", "other.test", "DNS:audit.test"));
    EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "audit.test", "other.test", "DNS:audit.test", 1));
}

TEST(Ruvia, postgresql_tls_accepts_1_1_and_rejects_1_0_without_relaxing_identity_or_record_limits) {
    database_tls_tests::checks_protocol_floor(ruvia::db_driver::postgresql);
}

TEST(Ruvia, postgresql_configured_tls_minimum_blocks_startup_on_older_tls) {
    database_tls_tests::checks_configured_protocol_floor(ruvia::db_driver::postgresql);
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
TEST(Ruvia, mariadb_tls_checks_ip_san_even_on_loopback_before_authentication) {
    using namespace database_tls_tests;
    const auto driver = ruvia::db_driver::mariadb;
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "127.0.0.1", ""));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "DNS:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2"));
    EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1"));
    EXPECT_TRUE(!sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1", 1));
}

TEST(Ruvia, mariadb_tls_accepts_1_1_and_rejects_1_0_without_relaxing_identity_or_record_limits) {
    database_tls_tests::checks_protocol_floor(ruvia::db_driver::mariadb);
}

TEST(Ruvia, mariadb_configured_tls_minimum_blocks_authentication_on_older_tls) {
    database_tls_tests::checks_configured_protocol_floor(ruvia::db_driver::mariadb);
}
#endif

// Public migration descriptors and options are shared by both SQL backends.

#if defined(RUVIA_ENABLE_POSTGRESQL) || defined(RUVIA_ENABLE_MARIADB)
namespace {

void check_unverifiable_history(ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver, std::span<const ruvia::db_migration> migrations) {
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
[[nodiscard]] std::optional<ruvia::db_error::code_type> interrupted_migration(const ruvia::db_config& config, ruvia::operation_options operation, ruvia::stop_source* stop, lock_requested_type lock_requested) {
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
        (void)ruvia::test::run_db_migration(ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}, bad_migrations);
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
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("SET")}, {.column_ = std::string("pg_advisory_lock"), .rows_ = std::vector<std::string>({std::string("")})}, {.column_ = std::string(""), .command_ = std::string("RESET")}, {.column_ = std::string(""), .command_ = std::string("CREATE TABLE")}, {.column_ = std::string("?column?"), .type_ = 23, .rows_ = std::vector<std::string>({std::string("1")})}, {.column_ = std::string("checksum"), .rows_ = std::vector<std::string>({std::string("")}), .null_rows_ = std::vector<bool>(1, null)}, {.column_ = std::string(""), .command_ = std::string("UPDATE 1")}, {.column_ = std::string("pg_advisory_unlock"), .type_ = 16, .rows_ = std::vector<std::string>({std::string("t")})}}));
        check_unverifiable_history(peer, ruvia::db_driver::postgresql, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_postgresql_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("SET")}, {.column_ = std::string("pg_advisory_lock"), .rows_ = std::vector<std::string>({std::string("")})}, {.column_ = std::string(""), .command_ = std::string("RESET")}, {.column_ = std::string(""), .command_ = std::string("CREATE TABLE")}, {.column_ = std::string("?column?"), .type_ = 23}, {.column_ = std::string(""), .command_ = std::string("ALTER TABLE")}, {.column_ = std::string("pg_advisory_unlock"), .type_ = 16, .rows_ = std::vector<std::string>({std::string("t")})}}));
    check_unverifiable_history(peer, ruvia::db_driver::postgresql, {});
}

// The migration lock belongs to the session that took it. A migration that
// leaves that session unfit for sharing -- here an unwrapped BEGIN leaving a
// transaction open -- ends it, and the history row must then fail rather than
// be written on a new session that never held the lock.
TEST(Ruvia, db_migrator_fails_once_its_locked_session_is_lost) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "BEGIN", .atomicity_ = ruvia::db_migration_atomicity::unwrapped})};
    peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>(
        {std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"}, {.column_ = "pg_advisory_lock", .rows_ = {""}}, {.column_ = "", .command_ = "RESET"}, {.column_ = "", .command_ = "CREATE TABLE"}, {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}}, {.column_ = "checksum"}, {.column_ = "", .command_ = "BEGIN", .status_ = 'T'}}),
            std::vector<peer_type::postgres_reply>(  // A replacement session, which never took the lock.
                {{.column_ = "", .command_ = "INSERT 0 1"}, {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}})}));
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
    ruvia::db_config config{.driver_ = ruvia::db_driver::postgresql, .host_ = "ruvia-migrator.invalid", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
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
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE t(id INT)", .atomicity_ = ruvia::db_migration_atomicity::unwrapped})};
    peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>({std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"}, {.column_ = "pg_advisory_lock", .rows_ = {""}}, {.column_ = "", .command_ = "RESET"}, {.column_ = "", .command_ = "CREATE TABLE"}, {.column_ = "?column?", .type_ = 23, .rows_ = {"1"}}, {.column_ = "checksum"}, {.column_ = "", .command_ = "CREATE TABLE"}, {.column_ = "", .command_ = "INSERT 0 1"}, {.column_ = "pg_advisory_unlock", .type_ = 16, .rows_ = {"t"}}})}));
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
        peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>({std::vector<peer_type::postgres_reply>({{.column_ = "", .command_ = "SET"}, {.column_ = "pg_advisory_lock", .rows_ = {""}, .hold_ = 1}})}));
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
        const auto code = cancel ? interrupted_migration(scripted_config(peer, ruvia::db_driver::postgresql), {.stop_token_ = stop.token()}, &stop, lock_requested) : interrupted_migration(scripted_config(peer, ruvia::db_driver::postgresql), {.timeout_ = std::chrono::milliseconds(200)}, nullptr, lock_requested);
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
    EXPECT_THROW((void)ruvia::db_migrator(ruvia::event_loop{}, ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}), std::invalid_argument);
    ruvia::event_loop_pool without_lookups({.loop_count_ = 2});
    EXPECT_THROW((void)ruvia::db_migrator(without_lookups.loop(0), ruvia::db_config{.driver_ = ruvia::db_driver::postgresql, .host_ = "db.example"}), std::invalid_argument);
    const ruvia::db_migrator migrator(without_lookups.loop(0), ruvia::db_config{.driver_ = ruvia::db_driver::postgresql, .host_ = "127.0.0.1", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}});
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
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("1")})}, {.column_ = std::string("")}, {.rows_ = std::vector<std::string>({std::string("1")})}, {.column_ = std::string("checksum"), .rows_ = std::vector<std::string>({std::string("")}), .null_rows_ = std::vector<bool>(1, null)}, {.column_ = std::string(""), .affected_rows_ = 1}, {.column_ = std::string("")}}), peer_type::after_startup::idle);
        check_unverifiable_history(peer, ruvia::db_driver::mariadb, migrations);
    }
}

TEST(Ruvia, db_migrator_rejects_mariadb_history_without_a_digest_column) {
    using peer_type = ruvia::test::backend_startup_peer;
    peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("1")})}, {.column_ = std::string("")}, {}, {.column_ = std::string("")}, {.column_ = std::string("")}}), peer_type::after_startup::idle);
    check_unverifiable_history(peer, ruvia::db_driver::mariadb, {});
}

TEST(Ruvia, db_migrator_applies_and_records_mariadb_migrations_on_the_callers_loop) {
    using peer_type = ruvia::test::backend_startup_peer;
    const std::array migrations{ruvia::db_migration({.id_ = "001", .sql_ = "CREATE TABLE IF NOT EXISTS t(id INT)"})};
    peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = {"1"}}, {.column_ = ""}, {.rows_ = {"1"}}, {.column_ = "checksum"}, {.column_ = ""}, {.column_ = "", .affected_rows_ = 1}, {.column_ = ""}}), peer_type::after_startup::idle);
    const auto report = ruvia::test::run_db_migration(scripted_config(peer, ruvia::db_driver::mariadb), migrations);
    ASSERT_EQ(report.applied().size(), std::size_t{1});
    EXPECT_EQ(report.applied()[0], "001");
    EXPECT_TRUE(report.skipped().empty());
}

TEST(Ruvia, db_migrator_mariadb_run_ends_on_cancellation_and_timeout) {
    using peer_type = ruvia::test::backend_startup_peer;
    for (const bool cancel : {true, false}) {
        SCOPED_TRACE(cancel ? "cancelled" : "timed out");
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = {"1"}, .hold_ = 1}}), peer_type::after_startup::idle);
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
        const auto code = cancel ? interrupted_migration(scripted_config(peer, ruvia::db_driver::mariadb), {.stop_token_ = stop.token()}, &stop, lock_requested) : interrupted_migration(scripted_config(peer, ruvia::db_driver::mariadb), {.timeout_ = std::chrono::milliseconds(200)}, nullptr, lock_requested);
        EXPECT_TRUE(code == (cancel ? ruvia::db_error::code_type::cancelled : ruvia::db_error::code_type::timeout));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
        // Answering the held lock only reaches a connection the run closed.
        peer.release_hold(1);
        peer.wait_for_disconnects();
    }
}
#endif
