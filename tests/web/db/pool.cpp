#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
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

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/db/db.h"

#include "backend_client_fixture.h"
#include "http_server_fixture.h"
#include "test_exceptions.h"
#include "worker_task_fixture.h"

namespace ruvia::test {

// Runs one migration to completion on a fresh loop, whose blocking pool looks
// a DNS host up, and returns its report or rethrows its failure.
[[nodiscard]] inline db_migration_report run_db_migration(const db_config& config,
    std::span<const db_migration> migrations, const db_migrator_options& options = {},
    operation_options operation = {}) {
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
        const ruvia::db_config config{
            .driver_ = driver, .host_ = "localhost", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
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
        peer_type peer(driver == ruvia::db_driver::postgresql ? peer_type::protocol::postgresql : peer_type::protocol::mariadb,
            false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>(), 3);
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
        peer_type peer(driver == ruvia::db_driver::postgresql ? peer_type::protocol::postgresql : peer_type::protocol::mariadb,
            false, peer_type::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>(), 2);
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
                        failed = error.code() == db_error::code_type::statement_failed &&
                                 error.sql_state() == "42S02";
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
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK")},
            {.rows_ = std::vector<std::string>({std::string("available")})},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}}),
        2);
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
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}}),
        2);
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
    auto cold_task = []() -> ruvia::task<void> { co_return; }();
    {
        auto operation = ruvia::make_scoped_operation(operation_scope, std::move(cold_task));
        EXPECT_TRUE(operation_scope.has_pending_operations());
    }
    EXPECT_TRUE(!operation_scope.has_pending_operations());
}

TEST(Ruvia, db_error_carries_category_and_native_diagnostics) {
    const ruvia::db_error timeout(ruvia::db_error::code_type::timeout,
        ruvia::db_driver::mariadb, "timeout", 1205, "HY000");
    const ruvia::db_error unique_violation(ruvia::db_error::code_type::statement_failed,
        ruvia::db_driver::postgresql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::db_error cancelled(ruvia::db_error::code_type::cancelled,
        ruvia::db_driver::postgresql, "cancelled");
    const ruvia::db_error closing(ruvia::db_error::code_type::closing,
        ruvia::db_driver::mariadb, "closing");
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
    for (const auto field : {&ruvia::db_config::connect_timeout_, &ruvia::db_config::read_timeout_,
             &ruvia::db_config::write_timeout_, &ruvia::db_config::query_timeout_,
             &ruvia::db_config::acquire_timeout_, &ruvia::db_config::transaction_timeout_}) {
        for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
            config.*field = invalid;
            EXPECT_TRUE(throws_on([&] { ruvia::db_migrator migrator(loop.loop(), config); }));
        }
        (config.*field).reset();
    }
}

TEST(Ruvia, db_migrator_rejects_non_positive_lock_timeouts) {
    const migrator_loop loop;
    ruvia::db_migrator_options options;
    for (const auto invalid : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
        options.lock_timeout_ = invalid;
        EXPECT_TRUE(throws_on([&] { ruvia::db_migrator migrator(loop.loop(), test_db_config(), options); }));
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
        rejected = std::string_view(error.what()) ==
                   "database migration ids must be unique, including case";
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
    for (const auto timeout : {
             ruvia::timeout_duration(std::chrono::duration<std::uint64_t, std::milli>{
                 std::numeric_limits<std::uint64_t>::max()}),
             ruvia::timeout_duration(std::chrono::milliseconds{
                 std::int64_t{std::numeric_limits<std::int32_t>::max()} + 1})}) {
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

        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(loop.loop(), config, options); }));
    }
    {
        auto config = test_db_config();
        config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        const ruvia::db_migrator_options options{
            .table_ = "invalid-table-name",
        };

        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(loop.loop(), config, options); }));
    }
}

TEST(Ruvia, db_migrator_rejects_nul_bytes_in_credentials) {
    using namespace std::string_literals;
    const migrator_loop loop;
    for (const auto field : {&ruvia::db_config::username_, &ruvia::db_config::password_, &ruvia::db_config::database_}) {
        auto config = test_db_config();
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        config.*field = "app\0suffix"s;
        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(loop.loop(), config, {}); }));
    }
}

TEST(Ruvia, database_config_rejects_non_host_dns_inputs) {
    const migrator_loop loop;
    for (const std::string_view host : {std::string_view("bad_name"), std::string_view("1.2.3.999"),
             std::string_view("host..example"), std::string_view("-host.example")}) {
        auto config = test_db_config();
        config.host_ = host;
        config.tls_.mode_ = ruvia::client_tls_mode::disabled;
        EXPECT_TRUE(throws_on([&] { (void)ruvia::db_migrator(loop.loop(), config, {}); }));
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
        destination.select(through_expressions
                               ? destination.import_expression(expressions.value(statement.params()[0]))
                               : destination.value(statement.params()[0]));
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
void check_session_ended_while_idle(
    ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver) {
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
            test::with_connected_db_client(attachment, db_config{.driver_ = driver},
                test::backend_startup_peer::after_startup::drop_mid_result,
                [&](db_client& client) -> task<void> {
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
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
            std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("before")}), .close_after_ = true}, {.rows_ = std::vector<std::string>({std::string("after")})}}));
        check_session_ended_while_idle(peer, ruvia::db_driver::postgresql);
    }
#endif
#ifdef RUVIA_ENABLE_MARIADB
    {
        peer_type peer(std::vector<peer_type::maria_reply>({{.rows_ = std::vector<std::string>({std::string("before")}), .close_after_ = true}, {.rows_ = std::vector<std::string>({std::string("after")})}}),
            peer_type::after_startup::reaccept);
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
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.error_ = std::string("duplicate key value violates unique constraint"), .sql_state_ = std::string("23505")},
            {.rows_ = std::vector<std::string>({std::string("kept")})}}));
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

// A statement that fails inside a transaction fails the transaction, and the
// pool rolls it back on the kept connection before the next caller gets it.
TEST(Ruvia, db_rejected_transaction_statement_rolls_back_on_the_kept_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.status_ = 'E', .error_ = std::string("syntax error at or near \"SELEC\"")},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK")},
            {.rows_ = std::vector<std::string>({std::string("after")})}}));
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
            test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
                test::backend_startup_peer::after_startup::reaccept, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.copy_ = copy}, {.rows_ = std::vector<std::string>({std::string("fresh")})}}));
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
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})}, {.rows_ = std::vector<std::string>({std::string("c")})}, {.rows_ = std::vector<std::string>({std::string("d")})}, {.rows_ = std::vector<std::string>({std::string("e")})},
            {.rows_ = std::vector<std::string>({std::string("f")})}}));
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
    for (const auto stale :
        {stale_case{"0A000", "RevalidateCachedQuery"}, stale_case{"26000", "FetchPreparedStatement"}}) {
        test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
            test::backend_startup_peer::after_startup::idle,
            std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})},
                {.error_ = std::string("stale statement"), .sql_state_ = std::string(stale.sql_state_), .routine_ = std::string(stale.routine_)},
                {.rows_ = std::vector<std::string>({std::string("c")})}}));
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
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'}, {.rows_ = std::vector<std::string>({std::string("a")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("b")}), .status_ = 'T'},
            {.status_ = 'E', .error_ = std::string("cached plan must not change result type"), .sql_state_ = std::string("0A000"), .routine_ = std::string("RevalidateCachedQuery")},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK")}, {.rows_ = std::vector<std::string>({std::string("c")})}}));
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
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("a")})}, {.rows_ = std::vector<std::string>({std::string("b")})},
            {.error_ = std::string("not supported"), .sql_state_ = std::string("0A000"), .routine_ = std::string("ExecInitNode")}, {.rows_ = std::vector<std::string>({std::string("c")})}}));
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
    handshake_peer(asio::ssl::context& tls, bool maria)
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
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
        thread_ = std::thread([this] { io_.run(); });
    }
    ~handshake_peer() {
        asio::post(io_, [this] { close(); });
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

bool sends_startup(ruvia::db_driver driver, const char* reference, const char* common_name,
    const char* san, std::uint64_t record_limit = std::uint64_t{1} << 23, int version = 0,
    bool elliptic_curve = false, bool mutual_tls = false, const char* cipher = nullptr,
    ruvia::tls_version minimum = ruvia::tls_version::tls_1_1) {
    ruvia::test::tls_identity identity(common_name, san, elliptic_curve);
    auto* const context = identity.context_.native_handle();
    if (version != 0) {
        // Only the adversarial test peer relaxes OpenSSL's legacy-signature
        // policy. The database client must retain the production policy.
        SSL_CTX_set_security_level(context, 0);
        if (SSL_CTX_set_min_proto_version(context, version) != 1 ||
            SSL_CTX_set_max_proto_version(context, version) != 1) {
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
        std::filesystem::permissions(identity.private_key_file(),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        config.tls_.identity_ = ruvia::tls_identity_config{
            .certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
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
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "127.0.0.1", "",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.2",
            limit, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            1, TLS1_1_VERSION, elliptic_curve));
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_VERSION, elliptic_curve));
        EXPECT_TRUE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_1_VERSION, elliptic_curve, true));
        const auto* const cbc = elliptic_curve ? "ECDHE-ECDSA-AES128-SHA" : "ECDHE-RSA-AES128-SHA";
        EXPECT_FALSE(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
            limit, TLS1_2_VERSION, elliptic_curve, false, cbc));
    }
}

void checks_configured_protocol_floor(ruvia::db_driver driver) {
    for (const auto minimum : {ruvia::tls_version::tls_1_2, ruvia::tls_version::tls_1_3}) {
        for (const int version : {TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(static_cast<int>(minimum));
            SCOPED_TRACE(version);
            EXPECT_EQ(sends_startup(driver, "127.0.0.1", "other.test", "IP:127.0.0.1",
                          std::uint64_t{1} << 23, version, false, false, nullptr, minimum),
                version >= static_cast<int>(minimum));
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

namespace db_repository_test {
using namespace ruvia;
namespace testing = ::ruvia::test;
RUVIA_DB_ENTITY(entity_type, "items", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string>)
RUVIA_DB_ENTITY(nullable_entity_type, "nullable_items",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string, db_column_options{.nullable_ = true}>)
RUVIA_DB_ENTITY(computed_entity_type, "computed_items",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string>,
    db_column<"name_length", std::int64_t, db_column_options{.generated_type_ = db_generated_type::stored}>)
RUVIA_DB_ENTITY(composite_entity_type, "composite_items",
    db_column<"tenant_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"item_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string>,
    db_column<"revision", std::int64_t>)
RUVIA_DB_ENTITY(numeric_array_entity, "numeric_arrays",
    db_column<"samples", std::vector<long double>, db_column_options{.data_type_ = db_data_type::numeric}>)

struct child;
struct parent;
RUVIA_DB_ENTITY(parent, "parents", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_one_to_many<"children", child, "parent">)
RUVIA_DB_ENTITY(child, "children", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"parent_id", std::int64_t>,
    db_many_to_one<"parent", parent, db_join_column<"parent_id", "id">>)

struct repository_runtime final {
    asio::io_context& context_ = test::new_test_io_context();
    event_loop_attachment attachment_ = attach_event_loop(context_, {.queue_capacity_ = 8});
};
db_config database_config() {
#ifdef RUVIA_ENABLE_POSTGRESQL
    return {.driver_ = db_driver::postgresql};
#else
    return {.driver_ = db_driver::mariadb};
#endif
}

RUVIA_DB_PROJECTION(item_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))
RUVIA_DB_PROJECTION(entity_with_rank,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(name, std::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

TEST(Ruvia, db_repository_projects_computed_dto_and_owns_expression_sources) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        for (int i = 0; i < 8; ++i) {
            {
                auto builder = repository.query_builder("i");
                {
                    db_query expressions;
                    auto label = expressions.call("concat", {expressions.column("name", "i"), expressions.value(std::string(200, 'x'))});
                    const db_order_term window_order[]{{expressions.column("id", "i")}};
                    auto rank = expressions.over(expressions.call("row_number"), {.order_by_ = window_order});
                    builder.select({{"id"}, {"label", label}, {"rank", rank}});
                }
                const auto statement = builder.get_query_and_parameters();
                EXPECT_TRUE(statement.sql().find(database_config().driver_ == db_driver::postgresql ? "\"row_number\"() OVER (ORDER BY" : "row_number() OVER (ORDER BY") != std::string_view::npos);
                EXPECT_EQ(statement.params().size(), std::size_t{1});
                auto many = builder.get_many<item_summary>();
                auto one = builder.get_one<item_summary>();
                auto page = builder.get_many_and_count<item_summary>();
                EXPECT_TRUE(testing::throws_on([&] { (void)builder.get_many(); }));
            }
        }
        auto partial = repository.query_builder();
        partial.select({{"name"}});
        auto result_value = partial.get_many();
        EXPECT_TRUE(testing::throws_on([&] { partial.select({{"missing"}}); }));
        EXPECT_TRUE(testing::throws_on([&] { partial.select({{"id"}, {"id"}}); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_write_validation_and_cte_rules_are_entry_point_independent) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto items = client.db().repository<entity_type>();
        const auto condition = entity_type::column<"id">() == 1;
        const entity_type empty_changes;
        auto builder = items.update_builder();
        EXPECT_TRUE(testing::throws_on([&] { builder.set(empty_changes); }));
        EXPECT_TRUE(testing::throws_on([&] { builder.set(std::span<const db_assignment>{}); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)items.update(condition, empty_changes); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)items.update(condition, std::span<const db_assignment>{}); }));

        auto parents = client.db().repository<parent>();
        auto hydrated = parents.query_builder("p");
        hydrated.left_join_and_select("children", "c");
        auto read_source = parents.query_builder("read");
        hydrated.with("read_source", read_source);
        auto mutation = parents.delete_builder();
        mutation.where(parent::column<"id">() == 1).returning();
        auto nested = parents.query_builder("changed");
        nested.with("deleted", mutation).from_cte("deleted");
        EXPECT_TRUE(testing::throws_on([&] { hydrated.with("mutation", mutation); }));
        EXPECT_TRUE(testing::throws_on([&] { hydrated.with("mutation", mutation.query()); }));
        EXPECT_TRUE(testing::throws_on([&] { hydrated.with("nested", nested); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_wildcard_projection_preserves_unknown_cte_columns_and_dialects) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto builder = repository.query_builder("samples");
        {
            db_query source;
            source.select(source.star()).from("measurements");
            std::string qualifier("samples");
            builder.with("samples", source).from_cte("samples").select({db_selection::star(qualifier), db_expr::value(5).as("rank")});
            qualifier.assign("destroyed");
        }
        const auto postgres = builder.query().compile(db_driver::postgresql);
        EXPECT_EQ(postgres.sql(), "WITH \"samples\" AS (SELECT * FROM \"measurements\") SELECT \"samples\".*, $1 AS \"rank\" FROM \"samples\" AS \"samples\"");
        EXPECT_EQ(postgres.params().size(), std::size_t{1});
        const auto maria = builder.query().compile(db_driver::mariadb);
        EXPECT_EQ(maria.sql(), "WITH `samples` AS (SELECT * FROM `measurements`) SELECT `samples`.*, ? AS `rank` FROM `samples` AS `samples`");
        EXPECT_EQ(maria.params().size(), std::size_t{1});
        EXPECT_TRUE(testing::throws_on([&] { builder.add_select(db_expr::value(6), "rank"); }));
        EXPECT_TRUE(testing::throws_on([&] { builder.select({db_selection{}}); }));
        EXPECT_TRUE(testing::throws_on([&] { builder.select({db_expr::value(1).as("")}); }));
        const auto unchanged = builder.query().compile(db_driver::postgresql);
        EXPECT_EQ(unchanged.sql(), postgres.sql());

        auto appended = repository.query_builder("s");
        appended.select({db_selection::star("s")}).add_select(db_expr::value(9), "rank");
        const auto added = appended.query().compile(db_driver::postgresql);
        EXPECT_EQ(added.sql(), "SELECT \"s\".*, $1 AS \"rank\" FROM \"items\" AS \"s\"");
        appended.select({db_expr::value(1).as("rank")}).add_select({db_selection::star("s")});
        const auto appended_wildcard = appended.query().compile(db_driver::postgresql);
        EXPECT_EQ(appended_wildcard.sql(),
            "SELECT $1 AS \"rank\", \"s\".* FROM \"items\" AS \"s\"");
        auto archive = repository.insert_builder();
        archive.insert_from({"id", "name"}, builder);
        EXPECT_TRUE(testing::throws_on([&] { archive.insert_from({"id", "unknown"}, builder); }));
        const auto inserted = archive.query().compile(db_driver::postgresql);
        EXPECT_TRUE(inserted.sql().find("INSERT INTO \"items\" (\"id\", \"name\")") != std::string_view::npos);
        EXPECT_TRUE(inserted.sql().find("SELECT \"samples\".*, $1 AS \"rank\"") != std::string_view::npos);
        co_return;
    });
}

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_repository_scalar_results_own_text_across_reads_and_returning) {
    const std::string first(192, 'a');
    const std::string second(256, 'b');
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string("name"), .rows_ = std::vector<std::string>({std::string(first)})},
            {.column_ = std::string("name"), .rows_ = std::vector<std::string>({std::string(first), std::string(second)})},
            {.column_ = std::string("name"), .rows_ = std::vector<std::string>({std::string(second)}), .command_ = std::string("UPDATE 1")},
            {.column_ = std::string("name"), .rows_ = std::vector<std::string>({std::string(second), std::string(first)}), .command_ = std::string("DELETE 2")},
            {.column_ = std::string("name"), .rows_ = std::vector<std::string>({std::string("raw")})},
            {.column_ = std::string("name"), .rows_ = std::vector<std::string>()}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<entity_type>();
        auto read = repository.query_builder("i");
        read.select({"name"});
        const auto one = co_await read.get_scalar<std::string>();
        const auto many = co_await read.get_scalars<string>();
        auto update = repository.update_builder();
        update.set("name", second).where(entity_type::column<"id">() == 1).returning({"name"});
        const auto returned_one = co_await update.get_scalar<string>();
        auto remove = repository.delete_builder();
        remove.where(entity_type::column<"id">() > 0).returning({"name"});
        const auto returned_many = co_await remove.get_scalars<std::string>();
        {
            const auto raw = co_await read.get_raw_many();
            EXPECT_EQ(raw.size(), std::size_t{1});
            if (!raw.empty()) {
                EXPECT_EQ(raw.front()["name"].as<std::string_view>().value_or(""), "raw");
            }
        }
        EXPECT_TRUE(!(co_await read.get_scalar<std::string>()).has_value());
        // Each mapped query has released its raw rows, and later queries have
        // reused the connection. All mapped strings must still own their data.
        EXPECT_TRUE(one.has_value());
        if (one) {
            EXPECT_EQ(std::string_view(*one), first);
        }
        EXPECT_EQ(many.size(), std::size_t{2});
        if (many.size() == 2) {
            EXPECT_EQ(many[0].view(), first);
            EXPECT_EQ(many[1].view(), second);
        }
        EXPECT_TRUE(returned_one.has_value());
        if (returned_one) {
            EXPECT_EQ(returned_one->view(), second);
        }
        EXPECT_EQ(returned_many.size(), std::size_t{2});
        if (returned_many.size() == 2) {
            EXPECT_EQ(std::string_view(returned_many[0]), second);
            EXPECT_EQ(std::string_view(returned_many[1]), first);
        }
    });
}

using nullable_text_array = std::vector<std::optional<std::string>>;
RUVIA_DB_ENTITY(array_entity, "array_items",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"values", nullable_text_array>)
RUVIA_DB_PROJECTION(array_projection,
    db_column<"values", std::vector<std::optional<string>>>)

TEST(Ruvia, db_repository_array_results_own_tokens_in_scalars_entities_and_projections) {
    const std::string long_text(256, 'x');
    const auto array_text = std::string("{\"") + long_text + "\",NULL,\"NULL\",\"comma,quote\\\"slash\\\\\",\"\"}";
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text)})},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text), std::string("{}")})},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text)}), .command_ = std::string("UPDATE 1")},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text), std::string("{}")}), .command_ = std::string("DELETE 2")},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text)})},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text)})},
            {.column_ = std::string("values"), .rows_ = std::vector<std::string>({std::string(array_text)})}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<array_entity>();
        auto read = repository.query_builder("a");
        read.select({"values"});
        const auto one = co_await read.get_scalar<nullable_text_array>();
        const auto many = co_await read.get_scalars<nullable_text_array>();
        auto update = repository.update_builder();
        update.set("id", 2).where(array_entity::column<"id">() == 1).returning({"values"});
        const auto returned_one = co_await update.get_scalar<nullable_text_array>();
        auto remove = repository.delete_builder();
        remove.where(array_entity::column<"id">() > 0).returning({"values"});
        const auto returned_many = co_await remove.get_scalars<nullable_text_array>();
        const auto entity = co_await read.get_one();
        const auto projection = co_await read.get_one<array_projection>();
        bool rejected_null = false;
        try {
            (void)co_await read.get_scalar<std::vector<std::string>>();
        } catch (const db_conversion_error& error) {
            rejected_null = error.code() == db_conversion_error::code_type::invalid_format;
        }
        EXPECT_TRUE(rejected_null);
        const auto check_array = [&](const nullable_text_array& values) {
            EXPECT_EQ(values.size(), std::size_t{5});
            if (values.size() == 5) {
                EXPECT_TRUE(values[0].has_value());
                EXPECT_TRUE(!values[1].has_value());
                EXPECT_TRUE(values[2].has_value());
                EXPECT_TRUE(values[3].has_value());
                EXPECT_TRUE(values[4].has_value());
                if (values[0] && values[2] && values[3] && values[4]) {
                    EXPECT_EQ(std::string_view(*values[0]), long_text);
                    EXPECT_EQ(*values[2], "NULL");
                    EXPECT_EQ(*values[3], "comma,quote\"slash\\");
                    EXPECT_TRUE(values[4]->empty());
                }
            }
        };
        EXPECT_TRUE(one.has_value());
        if (one) {
            check_array(*one);
        }
        EXPECT_EQ(many.size(), std::size_t{2});
        if (many.size() == 2) {
            check_array(many[0]);
            EXPECT_TRUE(many[1].empty());
        }
        EXPECT_TRUE(returned_one.has_value());
        if (returned_one) {
            check_array(*returned_one);
        }
        EXPECT_EQ(returned_many.size(), std::size_t{2});
        if (returned_many.size() == 2) {
            check_array(returned_many[0]);
            EXPECT_TRUE(returned_many[1].empty());
        }
        EXPECT_TRUE(entity.has_value());
        if (entity) {
            check_array(entity->get<"values">());
        }
        EXPECT_TRUE(projection.has_value());
        if (projection) {
            const auto& values = projection->get<"values">();
            EXPECT_EQ(values.size(), std::size_t{5});
            if (values.size() == 5) {
                EXPECT_TRUE(values[0].has_value());
                EXPECT_TRUE(!values[1].has_value());
                if (values[0]) {
                    EXPECT_EQ(values[0]->view(), long_text);
                }
            }
        }
    });
}

TEST(Ruvia, db_repository_wildcards_decode_result_names_and_reject_ambiguous_mapped_columns) {
    using reply = test::backend_startup_peer::postgres_reply;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<reply>({reply{.columns_ = std::vector<std::string>({"name", "rank", "unmapped"}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({"Ada", "7", "extra"})})},
            reply{.columns_ = std::vector<std::string>({"name", "rank", "unmapped"}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({"Grace", "8", "raw"})})},
            reply{.columns_ = std::vector<std::string>({"id", "name", "rank"}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({"9", "Linus", "6"})})},
            reply{.columns_ = std::vector<std::string>({"name"}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({"missing rank"})})},
            reply{.columns_ = std::vector<std::string>({"name", "name", "rank"}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({"first", "second", "7"})})},
            reply{.columns_ = std::vector<std::string>({"name", "rank"})}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<entity_type>();
        auto make_projection = [&] {
            db_query source;
            source.select(source.star()).from("measurements");
            auto builder = repository.query_builder("samples");
            builder.with("samples", source).from_cte("samples").select({db_selection::star("samples")});
            builder.add_select(db_expr::value(7), "rank");
            return builder;
        };
        auto operation = [&] {
            auto builder = make_projection();
            return builder.get_many<entity_with_rank>();
        }();
        const auto many = co_await std::move(operation);
        EXPECT_EQ(many.size(), std::size_t{1});
        if (!many.empty()) {
            EXPECT_TRUE(!many[0].is_set<"id">());
            EXPECT_EQ(many[0].get<"name">(), "Ada");
            EXPECT_EQ(many[0].get<"rank">(), std::int64_t{7});
        }
        auto builder = make_projection();
        const auto [entities, raw] = co_await builder.get_raw_and_entities();
        EXPECT_EQ(entities.size(), std::size_t{1});
        EXPECT_EQ(raw.size(), std::size_t{1});
        if (!entities.empty() && !raw.empty()) {
            EXPECT_EQ(entities[0].get<"name">(), "Grace");
            EXPECT_TRUE(!entities[0].is_set<"id">());
            EXPECT_EQ(raw[0]["unmapped"].value().value_or(""), "raw");
            EXPECT_EQ(raw[0]["rank"].as<std::int64_t>().value_or(0), std::int64_t{8});
        }
        auto returned_operation = [&] {
            auto remove = repository.delete_builder("d");
            remove.where(entity_type::column<"id">() == 9)
                .returning({db_selection::star("d"), db_expr::value(6).as("rank")});
            return remove.get_one<entity_with_rank>();
        }();
        const auto returned = co_await std::move(returned_operation);
        EXPECT_TRUE(returned.has_value());
        if (returned) {
            EXPECT_EQ(returned->get<"id">(), std::int64_t{9});
            EXPECT_EQ(returned->get<"name">(), "Linus");
            EXPECT_EQ(returned->get<"rank">(), std::int64_t{6});
        }
        bool missing = false;
        try {
            (void)co_await builder.get_one<entity_with_rank>();
        } catch (const db_conversion_error& error) {
            missing = error.code() == db_conversion_error::code_type::missing_column;
        }
        EXPECT_TRUE(missing);
        bool ambiguous = false;
        try {
            (void)co_await builder.get_many<entity_with_rank>();
        } catch (const db_conversion_error& error) {
            ambiguous = error.code() == db_conversion_error::code_type::invalid_format;
        }
        EXPECT_TRUE(ambiguous);
        EXPECT_TRUE(!(co_await builder.get_one<entity_with_rank>()).has_value());
    });
}

TEST(Ruvia, db_repository_consumes_selected_projection_rows_and_owns_selection) {
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string("label"), .rows_ = std::vector<std::string>({std::string("Ada")})},
            {.column_ = std::string("label"), .rows_ = std::vector<std::string>({std::string("Grace"), std::string("Linus")})},
            {.column_ = std::string("label"), .rows_ = std::vector<std::string>()}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<entity_type>();
        auto one_operation = [&] {
            auto builder = repository.query_builder("i");
            db_query expressions;
            builder.select({{"label", expressions.column("name", "i")}});
            return builder.get_one<item_summary>();
        }();
        const auto one = co_await std::move(one_operation);
        EXPECT_TRUE(one.has_value());
        if (one) {
            EXPECT_EQ(one->get<"label">(), "Ada");
            EXPECT_TRUE(!one->is_set<"id">());
            EXPECT_TRUE(!one->is_set<"rank">());
        }
        auto many_operation = [&] {
            auto builder = repository.query_builder("i");
            db_query expressions;
            builder.select({{"label", expressions.column("name", "i")}});
            return builder.get_many<item_summary>();
        }();
        const auto many = co_await std::move(many_operation);
        EXPECT_EQ(many.size(), std::size_t{2});
        if (many.size() == 2) {
            EXPECT_EQ(many[0].get<"label">(), "Grace");
            EXPECT_EQ(many[1].get<"label">(), "Linus");
            for (const auto& row : many) {
                EXPECT_TRUE(!row.is_set<"id">());
                EXPECT_TRUE(!row.is_set<"rank">());
            }
        }
        auto builder = repository.query_builder("i");
        db_query expressions;
        builder.select({{"label", expressions.column("name", "i")}});
        const auto empty = co_await builder.get_one<item_summary>();
        EXPECT_TRUE(!empty.has_value());
    });
}

TEST(Ruvia, db_repository_add_select_maps_computed_dto_columns_and_rejects_duplicate_names) {
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string("id"), .rows_ = std::vector<std::string>({std::string("7")})},
            {.column_ = std::string("id"), .rows_ = std::vector<std::string>()}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<entity_type>();

        auto explicit_qb = repository.query_builder("i");
        explicit_qb.select({"id"})
            .add_select(entity_type::column<"name">("i"), "label");
        EXPECT_TRUE(testing::throws_on([&] { explicit_qb.add_select("id"); }));
        EXPECT_TRUE(testing::throws_on([&] { explicit_qb.add_select(db_expr::value(1), "label"); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)explicit_qb.get_many(); }));
        bool missing_label = false;
        try {
            (void)co_await explicit_qb.get_one<item_summary>();
        } catch (const db_conversion_error& error) {
            missing_label = error.code() == db_conversion_error::code_type::missing_column;
        }
        EXPECT_TRUE(missing_label);

        auto default_qb = repository.query_builder("i");
        EXPECT_TRUE(testing::throws_on([&] { default_qb.add_select("id"); }));
        default_qb.add_select(db_expr::value(5), "rank");
        EXPECT_TRUE(testing::throws_on([&] { default_qb.add_select(db_expr::value(6), "rank"); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)default_qb.get_many(); }));
        const auto many = co_await default_qb.get_many<entity_with_rank>();
        EXPECT_TRUE(many.empty());
    });
}

TEST(Ruvia, db_repository_relation_query_reports_missing_mapped_columns_as_conversion_error) {
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string("id"), .rows_ = std::vector<std::string>({std::string("1")})}}));
    repository_runtime runtime;
    db_client client(runtime.attachment_.loop(), peer.config());
    test::drive_backend_client(runtime.attachment_, client, [&](db_client& connected) -> task<void> {
        auto builder = connected.db().repository<parent>().query_builder("p");
        builder.inner_join_and_select("children", "c");
        bool missing = false;
        try {
            (void)co_await builder.get_many();
        } catch (const db_conversion_error& error) {
            missing = error.code() == db_conversion_error::code_type::missing_column;
        }
        EXPECT_TRUE(missing);
    });
}

TEST(Ruvia, db_repository_relation_joins_validate_source_and_target_aliases) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto parents = client.db().repository<parent>();
        auto qb = parents.query_builder("p");
        EXPECT_TRUE(testing::throws_on([&] { qb.left_join("bogus.children", "c"); }));
        EXPECT_TRUE(testing::throws_on([&] { qb.inner_join("bogus.children", "c"); }));
        EXPECT_TRUE(testing::throws_on([&] { qb.left_join("children", "p"); }));
        qb.left_join("p.children", "c");
        EXPECT_TRUE(testing::throws_on([&] { qb.inner_join("children", "c"); }));
        const auto statement = qb.get_query_and_parameters();
        EXPECT_EQ(statement.sql(),
            R"(SELECT "p"."id" FROM "parents" AS "p" LEFT JOIN "children" AS "c" ON ("p"."id" = "c"."parent_id"))");
        co_return;
    });
}

TEST(Ruvia, db_repository_relation_path_rejects_alias_and_relation_name_ambiguity) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto qb = client.db().repository<parent>().query_builder("p");
        qb.left_join_and_select("children", "c");
        qb.left_join_and_select("c.parent", "children");
        // "children" is both a root relation and the alias of children.parent.
        EXPECT_TRUE(testing::throws_on([&] { qb.left_join_and_select("children.children", "grandchildren"); }));
        // An alias that names no root relation still resolves through the alias.
        qb.left_join_and_select("c.parent.children", "siblings");

        // A root alias that also names a root relation is ambiguous too.
        auto shadowed = client.db().repository<parent>().query_builder("children");
        EXPECT_TRUE(testing::throws_on([&] { shadowed.left_join_and_select("children.parent", "owner"); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_update_builder_owns_complex_conditions_and_cross_table_source) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto update = repository.update_builder("i");
        {
            db_query x;
            update.update_from<entity_type>("incoming")
                .set("name", x.column("name", "incoming"))
                .where(x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "incoming")))
                .and_where(x.binary(x.column("name", "i"), db_binary_operator::is_distinct_from, x.column("name", "incoming")))
                .returning({{"id"}, {"label", x.column("name", "i")}});
        }
        const auto statement = update.get_query_and_parameters();
        EXPECT_EQ(statement.sql(), "UPDATE \"items\" AS \"i\" SET \"name\" = \"incoming\".\"name\" FROM \"items\" AS \"incoming\" WHERE ((\"i\".\"id\" = \"incoming\".\"id\") AND (\"i\".\"name\" IS DISTINCT FROM \"incoming\".\"name\")) RETURNING \"i\".\"id\" AS \"id\", \"i\".\"name\" AS \"label\"");
        auto mapped = update.get_many<item_summary>();
        EXPECT_TRUE(testing::throws_on([&] { (void)update.execute(); }));
        db_query x;
        entity_type patch;
        patch.set<"name">("next");
        const auto condition = x.binary(x.column("name"), db_binary_operator::is_distinct_from, x.value("next"));
        auto ordinary = repository.update(condition, patch);
        auto returned = repository.update_builder().set(patch).where(condition).returning().get_many();
        auto expression_update = repository.update(condition, {{"name", x.value("next")}});
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.update(db_expression{}, patch); }));
        auto unbounded = repository.update_builder();
        unbounded.set(patch);
        EXPECT_TRUE(testing::throws_on([&] { (void)unbounded.get_query_and_parameters(); }));
        auto predicate = repository.update_builder("target");
        predicate.set(patch).where(entity_type::column<"id">() == 7);
        const auto predicate_statement = predicate.get_query_and_parameters();
        EXPECT_TRUE(predicate_statement.sql().find("\"target\".\"id\"") != std::string_view::npos);
        auto derived = repository.update_builder("i");
        {
            auto source_query = repository.query_builder("s");
            source_query.where(entity_type::column<"id">() > 3);
            derived.update_from(source_query, "incoming").set("name", x.column("name", "incoming")).where(x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "incoming")));
        }
        const auto derived_statement = derived.get_query_and_parameters();
        EXPECT_TRUE(derived_statement.sql().find("FROM (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"items\" AS \"s\" WHERE (\"s\".\"id\" > $1)) AS \"incoming\"") != std::string_view::npos);
        auto inserted = repository.insert_builder(patch);
        inserted.returning();
        const auto insert_statement = inserted.get_query_and_parameters();
        EXPECT_EQ(insert_statement.sql(), "INSERT INTO \"items\" (\"name\") VALUES ($1) RETURNING \"items\".\"id\", \"items\".\"name\"");
        auto removed = repository.delete_builder("old");
        removed.where(entity_type::column<"id">() == 9).returning();
        auto outer = repository.query_builder("removed");
        outer.with("removed", removed).from_cte("removed");
        const auto deletion = outer.get_query_and_parameters();
        EXPECT_TRUE(deletion.sql().find("WITH \"removed\" AS (DELETE FROM \"items\" AS \"old\" WHERE (\"old\".\"id\" = $1) RETURNING") == 0);
        co_return;
    });
}

RUVIA_DB_ENTITY(archive_type, "archive", db_column<"id", std::int64_t>, db_column<"name", std::string>)

TEST(Ruvia, db_repository_write_ctes_claim_and_persist_in_one_statement) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto archive = client.db().repository<archive_type>();
        for (int i = 0; i < 8; ++i) {
            {
                auto insert = archive.insert_builder();
                {
                    db_query x;
                    auto candidates = repository.query_builder("c");
                    candidates.where(entity_type::column<"name">() == "pending").order_by("id").limit(10).set_lock({.mode_ = db_row_lock::update, .skip_locked_ = true});
                    auto claim = repository.update_builder("t");
                    claim.update_from_cte("candidates", "c")
                        .set("name", x.value(std::string(300, 'x')))
                        .where(x.binary(x.column("id", "t"), db_binary_operator::equal, x.column("id", "c")))
                        .returning();
                    auto claimed = repository.query_builder("claimed");
                    claimed.from_cte("claimed");
                    insert.with("candidates", candidates).with("claimed", claim).insert_from({"id", "name"}, claimed).returning();
                    auto read = repository.query_builder("result");
                    read.with("candidates", candidates).with("claimed", claim).from_cte("claimed");
                    EXPECT_TRUE(testing::throws_on([&] { (void)read.get_many_and_count(); }));
                    EXPECT_TRUE(testing::throws_on([&] { (void)read.get_count(); }));
                    EXPECT_TRUE(testing::throws_on([&] { (void)read.subquery(x); }));
                    auto result_value = read.get_many();
                    auto invalid = archive.insert_builder();
                    EXPECT_TRUE(testing::throws_on([&] { invalid.insert_from({"id"}, claimed); }));
                    EXPECT_TRUE(testing::throws_on([&] { invalid.insert_from({"id", "unknown"}, claimed); }));
                }
                const auto statement = insert.get_query_and_parameters();
                EXPECT_TRUE(statement.sql().find("WITH \"candidates\" AS (SELECT") == 0);
                EXPECT_TRUE(statement.sql().find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
                EXPECT_TRUE(statement.sql().find("\"claimed\" AS (UPDATE \"items\" AS \"t\"") != std::string_view::npos);
                EXPECT_TRUE(statement.sql().find("INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"claimed\".\"id\", \"claimed\".\"name\" FROM \"claimed\" AS \"claimed\" RETURNING \"archive\".\"id\", \"archive\".\"name\"") != std::string_view::npos);
                EXPECT_EQ(statement.params().size(), std::size_t{3});
                auto operation = insert.get_many();
            }
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_write_builder_cancelled_operations_release_input_snapshots) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        stop_source stop;
        stop.request_stop();
        auto repository = client.db().with_options({.stop_token_ = stop.token()}).repository<entity_type>();
        for (int i = 0; i < 8; ++i) {
            bool cancelled = false;
            auto exercise = [&]() -> task<void> {
                auto operation = [&] {
                    db_query x;
                    auto update = repository.update_builder();
                    update.set("name", x.value(std::string(400, 'v')))
                        .where(x.binary(x.column("id"), db_binary_operator::is_distinct_from, x.value(1)))
                        .returning();
                    auto outer = repository.query_builder("updated");
                    outer.with("updated", update).from_cte("updated");
                    return outer.get_many();
                }();
                try {
                    auto result_value = co_await std::move(operation);
                } catch (const db_error& error) {
                    cancelled = error.code() == db_error::code_type::cancelled;
                }
            };
            co_await exercise();
            EXPECT_TRUE(cancelled);
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_imports_raw_correlated_query_joins_and_writer_parameters) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto read = repository.query_builder("i");
        auto write = repository.update_builder("i");
        write.set("name", "changed").update_from_cte("incoming", "n").where(entity_type::column<"id">() > 8);
        {
            db_query cte;
            cte.values({cte.value(2)});
            const borrowed_text cte_columns[]{"floor"};
            read.with("bounds", cte, {.columns_ = cte_columns});
            db_query source;
            source.select(source.column("id", "s")).from("samples", "s").where(source.binary(source.column("owner_id", "s"), db_binary_operator::equal, source.column("id", "i"))).and_where(source.binary(source.column("score", "s"), db_binary_operator::greater, source.value(3)));
            const db_source_column columns[]{{.name_ = "sample_id"}};
            const auto on = (entity_type::column<"id">() == db_expr::column("sample_id", "matched")) &&
                            (db_expr::column("sample_id", "matched") > 5);
            read.left_join(source, "matched", on, {.lateral_ = true, .columns_ = columns});
            const borrowed_text incoming_columns[]{"id"};
            write.with("incoming", cte, {.columns_ = incoming_columns});
            db_query writer_source;
            writer_source.select(writer_source.column("id", "s")).from("samples", "s").where(writer_source.binary(writer_source.column("owner_id", "s"), db_binary_operator::equal, writer_source.column("id", "n"))).and_where(writer_source.binary(writer_source.column("score", "s"), db_binary_operator::greater, writer_source.value(3)));
            const auto writer_on = (db_expr::column("id", "n") == db_expr::column("sample_id", "matched")) &&
                                   (db_expr::column("sample_id", "matched") > 5);
            write.join(db_join_type::inner, writer_source, "matched", writer_on, {.lateral_ = true, .columns_ = columns});
            source.where(source.value(false));
        }
        const auto statement = read.get_query_and_parameters();
        EXPECT_EQ(statement.sql(),
            "WITH \"bounds\" (\"floor\") AS (VALUES ($1)) SELECT \"i\".\"id\", \"i\".\"name\" FROM \"items\" AS \"i\" LEFT JOIN LATERAL (SELECT \"s\".\"id\" FROM \"samples\" AS \"s\" WHERE ((\"s\".\"owner_id\" = \"i\".\"id\") AND (\"s\".\"score\" > $2))) AS \"matched\" (\"sample_id\") ON ((\"i\".\"id\" = \"matched\".\"sample_id\") AND (\"matched\".\"sample_id\" > $3))");
        EXPECT_EQ(statement.params().size(), std::size_t{3});
        const auto update = write.get_query_and_parameters();
        EXPECT_TRUE(update.sql().find("SET \"name\" = $2 FROM \"incoming\" AS \"n\" INNER JOIN LATERAL (SELECT") != std::string_view::npos);
        EXPECT_TRUE(update.sql().find("\"s\".\"score\" > $3") != std::string_view::npos);
        EXPECT_TRUE(update.sql().find("\"matched\".\"sample_id\" > $4") != std::string_view::npos);
        EXPECT_TRUE(update.sql().find("WHERE (\"i\".\"id\" > $5)") != std::string_view::npos);
        EXPECT_EQ(update.params().size(), std::size_t{5});
        EXPECT_TRUE(testing::throws_on([&] { (void)read.query().compile(db_driver::mariadb); }));

        db_query plain;
        plain.select(plain.value(11));
        auto inner = repository.query_builder("root");
        inner.inner_join(plain, "v", entity_type::column<"id">() > 1);
        const auto inner_statement = inner.get_query_and_parameters();
        EXPECT_EQ(inner_statement.sql(),
            "SELECT \"root\".\"id\", \"root\".\"name\" FROM \"items\" AS \"root\" INNER JOIN (SELECT $1) AS \"v\" ON (\"root\".\"id\" > $2)");
        EXPECT_TRUE(testing::throws_on([&] { inner.join(db_join_type::left, plain, ""); }));
        db_query dml;
        dml.delete_from("samples").returning({dml.column("id")});
        EXPECT_TRUE(testing::throws_on([&] { inner.left_join(dml, "bad"); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_public_query_views_compose_all_writes_into_owned_query_ctes) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        db_query outer;
        {
            entity_type entity;
            entity.set<"id">(1);
            entity.set<"name">("new");
            auto insert = repository.insert_builder(entity);
            insert.returning({"id"});
            outer.with("inserted", insert.query());
            entity.set<"name">("mutated");
            insert.values(entity);

            auto update = repository.update_builder("u");
            update.set("name", "updated").where(entity_type::column<"id">() == 2).returning({db_selection::star("u"), db_expr::value(6).as("rank")});
            outer.with("updated", std::as_const(update).query());
            auto remove = repository.delete_builder("d");
            remove.where(entity_type::column<"id">() == 3).returning({"id"});
            outer.with("removed", remove.query());
            auto upsert = repository.insert_builder(entity);
            const borrowed_text conflicts[]{"id"};
            upsert.on_conflict({.conflict_paths_ = conflicts}).returning({"id"});
            outer.with("upserted", upsert.query());

            auto unbounded = repository.delete_builder();
            EXPECT_TRUE(testing::throws_on([&] { outer.with("unsafe", unbounded.query()); }));
            EXPECT_TRUE(testing::throws_on([&] { (void)entity_type::column<"id">().in(remove); }));
            EXPECT_TRUE(testing::throws_on([&] { (void)entity_type::column<"id">().not_in(remove); }));
            db_values_builder alternatives({"id"});
            alternatives.row({db_expr::value(4)});
            db_query membership;
            membership.select(membership.star()).from("items").where((entity_type::column<"id">().in(alternatives) && entity_type::column<"id">().not_in(alternatives)).expression(membership));
            const auto values_membership = membership.compile(db_driver::postgresql);
            EXPECT_EQ(values_membership.sql(),
                "SELECT * FROM \"items\" WHERE ((\"items\".\"id\" IN (VALUES ($1))) AND (\"items\".\"id\" NOT IN (VALUES ($2))))");
            EXPECT_EQ(values_membership.params().size(), std::size_t{2});
            auto ids = repository.query_builder("ids");
            ids.select({"id"}).where(entity_type::column<"id">() > 4);
            outer.select(outer.star("updated")).from("updated").where((entity_type::column<"id">("updated").in(ids) && entity_type::column<"id">("updated").not_in(ids)).expression(outer));
        }
        const auto statement = outer.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(),
            "WITH \"inserted\" AS (INSERT INTO \"items\" (\"id\", \"name\") VALUES ($1, $2) RETURNING \"items\".\"id\" AS \"id\"), "
            "\"updated\" AS (UPDATE \"items\" AS \"u\" SET \"name\" = $3 WHERE (\"u\".\"id\" = $4) RETURNING \"u\".*, $5 AS \"rank\"), "
            "\"removed\" AS (DELETE FROM \"items\" AS \"d\" WHERE (\"d\".\"id\" = $6) RETURNING \"d\".\"id\" AS \"id\"), "
            "\"upserted\" AS (INSERT INTO \"items\" (\"id\", \"name\") VALUES ($7, $8) ON CONFLICT (\"id\") DO UPDATE SET \"name\" = excluded.\"name\" RETURNING \"items\".\"id\" AS \"id\") "
            "SELECT \"updated\".* FROM \"updated\" WHERE ((\"updated\".\"id\" IN (SELECT \"ids\".\"id\" AS \"id\" FROM \"items\" AS \"ids\" WHERE (\"ids\".\"id\" > $9))) AND (\"updated\".\"id\" NOT IN (SELECT \"ids\".\"id\" AS \"id\" FROM \"items\" AS \"ids\" WHERE (\"ids\".\"id\" > $10))))");
        EXPECT_EQ(statement.params().size(), std::size_t{10});
        const auto literal = outer.compile(db_driver::postgresql, db_parameter_mode::literal);
        EXPECT_TRUE(literal.sql().find("VALUES (1, E'new')") != std::string_view::npos);
        EXPECT_TRUE(literal.sql().find("VALUES (1, E'mutated')") != std::string_view::npos);
        EXPECT_TRUE(literal.sql().find("SET \"name\" = E'updated'") != std::string_view::npos);
        EXPECT_TRUE(literal.params().empty());
        co_return;
    });
}

TEST(Ruvia, db_repository_composes_entity_joins_lateral_cte_and_grouped_queries) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto builder = repository.query_builder("i");
        {
            db_query x;
            auto source_value = repository.query_builder("s");
            source_value.where(entity_type::column<"id">() > 4);
            source_value.select({{"id"}});
            builder.with("recent", source_value, {.materialization_ = db_materialization::materialized});
            builder.join_cte(db_join_type::inner, "recent", "r", x.binary(x.column("id", "i"), db_binary_operator::equal, x.column("id", "r")));
            auto correlated = repository.query_builder("c");
            correlated.where(x.binary(x.column("id", "c"), db_binary_operator::equal, x.column("id", "i"))).limit(1);
            builder.join(db_join_type::left, correlated, "l", x.value(true), {.lateral_ = true});
            builder.join<entity_type>(db_join_type::cross, "other");
            builder.select({{"id"}, {"rank", x.aggregate("count", {x.star()})}});
            builder.group_by({x.column("id", "i")}).having(x.binary(x.aggregate("count", {x.star()}), db_binary_operator::greater, x.value(1)));
            builder.and_where(source_value.exists(x));
            builder.and_where(x.binary(x.column("id", "i"), db_binary_operator::in, source_value.subquery(x)));
        }
        const auto statement = builder.get_query_and_parameters();
        EXPECT_TRUE(statement.sql().find("WITH \"recent\" AS MATERIALIZED") != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find("LEFT JOIN LATERAL") != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find("CROSS JOIN \"items\" AS \"other\"") != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find("GROUP BY \"i\".\"id\" HAVING") != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find("EXISTS (SELECT") != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find(" IN (SELECT") != std::string_view::npos);
        auto operation = builder.get_many<item_summary>();
        co_return;
    });
}

TEST(Ruvia, db_repository_grouped_projection_reuses_imported_parameters) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto builder = client.db().repository<entity_type>().query_builder("i");
        {
            db_query x;
            const auto bucket = x.binary(x.column("id", "i"), db_binary_operator::add, x.value(3));
            builder.select({{"id", bucket}}).group_by({bucket});
            builder.having(x.binary(bucket, db_binary_operator::greater, x.value(10)));
        }
        const auto statement = builder.get_query_and_parameters();
        EXPECT_EQ(statement.sql(), "SELECT (\"i\".\"id\" + $1) AS \"id\" FROM \"items\" AS \"i\" GROUP BY (\"i\".\"id\" + $1) HAVING ((\"i\".\"id\" + $1) > $2)");
        EXPECT_EQ(statement.params().size(), std::size_t{2});
        co_return;
    });
}

TEST(Ruvia, db_repository_expression_writes_own_sources_and_validate_returning) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        entity_type entity;
        entity.set<"id">(8);
        entity.set<"name">("initial");
        EXPECT_TRUE(testing::throws_on([&] {
            auto bad = repository.insert_builder(entity);
            bad.returning();
            (void)bad.get_many<item_summary>();
        }));
        auto where = entity_type::column<"id">() == 8;
        for (int i = 0; i < 12; ++i) {
            {
                auto update = [&] {
                    db_query x;
                    const std::array changes{db_assignment{"name", x.call("concat", {x.column("name"), x.value(std::string(400, 'z'))})}};
                    const std::array fields_value{db_selection{"id"}, db_selection{"label", x.call("upper", {x.column("name")})}};
                    auto builder = repository.update_builder();
                    builder.set(changes).where(where).returning(fields_value);
                    return builder.get_many<item_summary>();
                }();
                auto insert = repository.insert_builder(entity).returning().get_many();
                auto erase = repository.delete_builder().where(where).returning().get_many();
                auto plain_update = repository.update_builder().set(entity).where(where).returning().get_many();
                db_query x;
                const std::array<borrowed_text, 1> conflict_paths{"id"};
                const db_assignment name_update{"name", x.call("concat", {x.excluded("name"), x.value("suffix")})};
                const std::array<db_assignment, 2> duplicate_updates{name_update, name_update};
                db_upsert_options options{
                    .conflict_paths_ = conflict_paths,
                    .skip_update_if_no_values_changed_ = true,
                    .update_expressions_ = std::span<const db_assignment>(duplicate_updates.data(), 1),
                    .update_where_ = x.binary(x.column("id", "items"), db_binary_operator::greater, x.value(0)),
                };
                auto upsert = repository.insert_builder(entity).on_conflict(options).returning().get_many();
                auto direct_result = repository.update(where, {{"name", x.value("replacement")}});
                EXPECT_TRUE(testing::throws_on([&] { (void)repository.update(where, {{"missing", x.value(1)}}); }));
                auto repeated = repository.update(where, {{"id", x.value(1)}, {"id", x.value(2)}});
                auto repeated_builder = repository.update_builder();
                repeated_builder.set({{"id", x.value(1)}, {"id", x.value(2)}}).where(where);
                const auto repeated_sql = repeated_builder.query().compile(db_driver::postgresql, db_parameter_mode::literal);
                EXPECT_TRUE(repeated_sql.sql().find("SET \"id\" = 2") != std::string_view::npos);
                EXPECT_TRUE(repeated_sql.sql().find("SET \"id\" = 1") == std::string_view::npos);
                options.update_expressions_ = duplicate_updates;
                EXPECT_TRUE(testing::throws_on([&] { (void)repository.upsert(entity, options); }));
            }
        }
        co_return;
    });
}
#endif

TEST(Ruvia, db_repository_builder_binds_entity_predicates_to_join_alias) {
    repository_runtime runtime;
    const auto config = database_config();
    test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto builder = repository.query_builder("i");
        builder.where(entity_type::column<"id">() >= 7).and_where(entity_type::column<"name">() == std::string("temporary"));
        builder.order_by("id").limit(3);
        const auto statement = builder.get_query_and_parameters();
        if (config.driver_ == db_driver::postgresql) {
            EXPECT_EQ(statement.sql(), "SELECT \"i\".\"id\", \"i\".\"name\" FROM \"items\" AS \"i\" WHERE ((\"i\".\"id\" >= $1) AND (\"i\".\"name\" = $2)) ORDER BY \"i\".\"id\" ASC LIMIT $3");
        } else {
            EXPECT_TRUE(statement.sql().find("WHERE ((`i`.`id` >= ?) AND (`i`.`name` = ?))") != std::string_view::npos);
        }
        EXPECT_EQ(statement.params().size(), std::size_t{3});
        co_return;
    });
}

TEST(Ruvia, db_repository_rejects_unbounded_writes_and_missing_primary_key) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        entity_type changes;
        changes.set<"name">("new");
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.update({}, changes); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.delete_by({}); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.remove(changes); }));
        const db_find_order missing_order[]{{"missing"}};
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.find({.order_ = missing_order}); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.increment({}, "id", 1); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.increment(entity_type::column<"id">() == 1, "name", 1); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.decrement(entity_type::column<"id">() == 1, "missing", 1); }));
        std::array<entity_type, 2> batch;
        batch[0].set<"id">(1);
        batch[0].set<"name">("new");
        batch[1].set<"id">(2);
        const borrowed_text conflict_paths[]{"id"};
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.upsert(batch, {.conflict_paths_ = conflict_paths}); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_rejects_writes_that_only_target_computed_columns) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<computed_entity_type>();
        computed_entity_type changes;
        changes.set<"name_length">(3);
        auto predicate = computed_entity_type::column<"id">() == 1;
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.update(predicate, changes); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.increment(predicate, "name_length", 1); }));
        const borrowed_text update_columns[]{"name_length"};
        EXPECT_TRUE(testing::throws_on([&] { (void)repository.upsert(changes, {.update_columns_ = update_columns}); }));

        computed_entity_type input;
        input.set<"id">(1);
        input.set<"name">("Ada");
        input.set<"name_length">(999);
        {
            const auto operation = repository.insert(input);
        }
        co_return;
    });
}

RUVIA_DB_ENTITY(key_only_type, "keys", db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>)

TEST(Ruvia, db_repository_conditional_upsert_owns_options_and_releases_cold_storage) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        repository_runtime runtime;
        const db_config config{.driver_ = driver};
        test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
            auto repository = client.db().repository<entity_type>();
            entity_type input;
            input.set<"id">(1);
            input.set<"name">(std::string(400, 'n'));
            const borrowed_text conflict_paths[]{"id"};
            for (int i = 0; i < 12; ++i) {
                if (config.driver_ == db_driver::postgresql) {
                    {
                        const auto operation = repository.upsert(input, {.conflict_paths_ = conflict_paths,
                                                                            .skip_update_if_no_values_changed_ = true,
                                                                            .index_predicate_ = entity_type::column<"name">().is_not_null()});
                    }
                } else {
                    EXPECT_TRUE(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .skip_update_if_no_values_changed_ = true}); }));
                    EXPECT_TRUE(testing::throws_on([&] { (void)repository.upsert(input, {.any_unique_key_ = true, .index_predicate_ = entity_type::column<"name">().is_not_null()}); }));
                }
            }
            auto keys = client.db().repository<key_only_type>();
            key_only_type key;
            key.set<"id">(1);
            {
                db_upsert_options options;
                if (config.driver_ == db_driver::postgresql) {
                    options.conflict_paths_ = conflict_paths;
                } else {
                    options.any_unique_key_ = true;
                }
                if (config.driver_ == db_driver::postgresql) {
                    const auto operation = keys.upsert(key, options);
                } else {
                    EXPECT_TRUE(testing::throws_on([&] { (void)keys.upsert(key, options); }));
                }
            }
            co_return;
        });
    }
}

TEST(Ruvia, db_repository_builder_orders_entity_fields_and_owns_predicates) {
    repository_runtime runtime;
    const auto config = database_config();
    test::with_connected_db_client(runtime.attachment_, config, [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        auto builder = repository.query_builder("i");
        builder.where(entity_type::column<"id">() >= 1)
            .and_where(entity_type::column<"name">().is_not_null())
            .or_where(entity_type::column<"id">() == 2)
            .order_by("name", db_order_direction::desc, db_nulls_order::last)
            .add_order_by("id", db_order_direction::asc, db_nulls_order::first)
            .offset(2)
            .limit(3);
        builder.cache("builder-page", std::chrono::seconds(5));
        const auto statement = builder.get_query_and_parameters();
        const auto sql = statement.sql();
        if (config.driver_ == db_driver::postgresql) {
            EXPECT_TRUE(sql.find("ORDER BY \"i\".\"name\" DESC NULLS LAST, \"i\".\"id\" ASC NULLS FIRST") != std::string_view::npos);
        } else {
            EXPECT_TRUE(sql.find("ORDER BY") != std::string_view::npos);
            EXPECT_TRUE(sql.find("LIMIT ? OFFSET ?") != std::string_view::npos);
        }
        EXPECT_EQ(statement.params().size(), std::size_t{4});
        EXPECT_TRUE(testing::throws_on([&] { builder.order_by("unknown"); }));
        EXPECT_TRUE(testing::throws_on([&] { builder.add_order_by("unknown"); }));
        // Validation precedes mutation: rejected fields leave the valid plan intact.
        const auto after_rejected_order = builder.get_query_and_parameters();
        EXPECT_EQ(after_rejected_order.sql(), sql);

        auto locked = repository.query_builder("i");
        const borrowed_text lock_tables[]{"i"};
        locked.set_lock({.mode_ = db_row_lock::share, .nowait_ = true, .tables_ = lock_tables});
        const auto lock_statement = locked.get_query_and_parameters();
        if (config.driver_ == db_driver::postgresql) {
            EXPECT_TRUE(lock_statement.sql().find("FOR SHARE OF \"i\" NOWAIT") != std::string_view::npos);
        } else {
            EXPECT_TRUE(lock_statement.sql().find("LOCK IN SHARE MODE") != std::string_view::npos);
        }
        auto relation = client.db().repository<parent>().query_builder("p");
        relation.inner_join_and_select("children", "c");
        const auto relation_statement = relation.get_query_and_parameters();
        EXPECT_TRUE(relation_statement.sql().find("INNER JOIN") != std::string_view::npos);
        {
            auto many = builder.get_many();
            auto one = builder.get_one();
            auto count = builder.get_count();
            auto exists = builder.get_exists();
            auto page = builder.get_many_and_count();
            entity_type patch;
            patch.set<"name">("cold");
            auto write = repository.update(entity_type::column<"id">() == 1, patch);
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_write_inputs_cover_default_sparse_nullable_and_composite_keys) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();

        // A single entity with no explicit values uses DEFAULT VALUES. Bulk
        // default-only input is rejected because there is no row width to encode.
        {
            entity_type defaults;
            auto operation = repository.insert(defaults);
        }
        std::array<entity_type, 2> sparse;
        sparse[0].set<"id">(1);
        sparse[1].set<"name">("only-name");
        auto sparse_insert = repository.insert(std::span<const entity_type>(sparse));
        EXPECT_TRUE(testing::throws_on([&] {
            std::array<entity_type, 2> defaults;
            (void)repository.insert(std::span<const entity_type>(defaults));
        }));

        auto nullable_repository = client.db().repository<nullable_entity_type>();
        nullable_entity_type nullable;
        nullable.set<"id">(7);
        nullable.set_null<"name">();
        auto update = nullable_repository.update(nullable_entity_type::column<"id">() == 7, nullable);
        auto increment = nullable_repository.increment(nullable_entity_type::column<"id">() == 7, "id", 1.5);
        auto decrement = nullable_repository.decrement(nullable_entity_type::column<"id">() == 7, "id", 1);
        auto deleted = nullable_repository.delete_by(nullable_entity_type::column<"id">() == 7);
        (void)sparse_insert;
        (void)update;
        (void)increment;
        (void)decrement;
        (void)deleted;

        auto composite = client.db().repository<composite_entity_type>();
        composite_entity_type key;
        key.set<"tenant_id">(10);
        key.set<"item_id">(20);
        auto removed = composite.remove(key);
        composite_entity_type partial;
        partial.set<"tenant_id">(10);
        EXPECT_TRUE(testing::throws_on([&] { (void)composite.remove(partial); }));
        co_return;
    });
}

TEST(Ruvia, db_repository_insert_batches_merge_columns_without_reinterpreting_rows) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        std::array<entity_type, 2> entities;
        entities[0].set<"id">(17);
        entities[1].set<"name">("second");
        auto batch = repository.insert_builder(std::span<const entity_type>(entities));
        auto appended = repository.insert_builder(entities[0]);
        appended.values(entities[1]);
        auto reversed = repository.insert_builder(entities[1]);
        reversed.values(entities[0]);
        db_query expected_rows;
        expected_rows.insert_into("items", {"id", "name"})
            .values({expected_rows.value(17), expected_rows.default_value()})
            .values({expected_rows.default_value(), expected_rows.value("second")});
        db_query expected_reversed;
        expected_reversed.insert_into("items", {"name", "id"})
            .values({expected_reversed.value("second"), expected_reversed.default_value()})
            .values({expected_reversed.default_value(), expected_reversed.value(17)});
        for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
            const auto expected = batch.query().compile(driver, db_parameter_mode::literal);
            const auto actual = appended.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(actual.sql(), expected.sql());
            const auto positioned = expected_rows.compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(actual.sql(), positioned.sql());
            const auto reverse = reversed.query().compile(driver, db_parameter_mode::literal);
            const auto positioned_reverse = expected_reversed.compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(reverse.sql(), positioned_reverse.sql());
            const auto bound = appended.query().compile(driver);
            EXPECT_EQ(bound.params().size(), std::size_t{2});
        }
        const borrowed_text keys[]{"id"};
        const auto driver = database_config().driver_;
        reversed.on_conflict(driver == db_driver::postgresql
                                 ? db_upsert_options{.conflict_paths_ = keys}
                                 : db_upsert_options{.any_unique_key_ = true});
        const auto upsert = reversed.get_query_and_parameters();
        EXPECT_TRUE(upsert.sql().find(driver == db_driver::postgresql
                                          ? "\"name\" = excluded.\"name\""
                                          : "`name` = VALUES(`name`)") != std::string_view::npos);
        co_return;
    });
}

TEST(Ruvia, db_repository_numeric_arrays_preserve_floating_precision_and_range) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<numeric_array_entity>();
        for (const auto input : {std::nextafter(1.0L, 2.0L), std::numeric_limits<long double>::max(),
                 std::numeric_limits<long double>::lowest()}) {
            numeric_array_entity entity;
            entity.set<"samples">(std::vector<long double>({input}));
            auto builder = repository.insert_builder(entity);
            const auto statement = builder.query().compile(db_driver::postgresql, db_parameter_mode::literal);
            const auto sql = statement.sql();
            const auto opening = sql.find('{');
            const auto closing = sql.find('}', opening);
            EXPECT_NE(opening, std::string_view::npos);
            EXPECT_NE(closing, std::string_view::npos);
            if (opening == std::string_view::npos || closing == std::string_view::npos) {
                co_return;
            }
            long double decoded{};
            const auto end = sql.data() + closing;
            const auto parsed = std::from_chars(sql.data() + opening + 1, end, decoded);
            EXPECT_EQ(parsed.ec, std::errc{});
            EXPECT_EQ(parsed.ptr, end);
            EXPECT_EQ(decoded, input);
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_nonfinite_array_elements_preserve_prior_insert_rows) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<numeric_array_entity>();
        numeric_array_entity entity;
        entity.set<"samples">(std::vector<long double>({2.0L}));
        auto builder = repository.insert_builder(entity);
        const auto before = builder.query().compile(db_driver::postgresql, db_parameter_mode::literal);
        for (const auto invalid : {std::numeric_limits<long double>::infinity(),
                 -std::numeric_limits<long double>::infinity(), std::numeric_limits<long double>::quiet_NaN()}) {
            entity.set<"samples">(std::vector<long double>({1.0L, invalid}));
            EXPECT_THROW(builder.values(entity), std::invalid_argument);
            const auto after = builder.query().compile(db_driver::postgresql, db_parameter_mode::literal);
            EXPECT_EQ(after.sql(), before.sql());
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_insert_defaults_and_rejected_batches_preserve_existing_rows) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        std::array<entity_type, 3> entities;
        entities[1].set<"name">("explicit");
        auto appended = repository.insert_builder(entities[0]);
        for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
            const auto before = appended.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(before.sql(), driver == db_driver::postgresql
                                        ? "INSERT INTO \"items\" DEFAULT VALUES"
                                        : "INSERT INTO `items` () VALUES ()");
            EXPECT_TRUE(testing::throws_on([&] { appended.values(std::span<const entity_type>{}); }));
            EXPECT_TRUE(testing::throws_on([&] { appended.values(entities[2]); }));
            const auto after = appended.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(after.sql(), before.sql());
        }
        appended.values(entities[1]).values(entities[2]);
        auto batch = repository.insert_builder(std::span<const entity_type>(entities));
        db_query expected_rows;
        expected_rows.insert_into("items", {"name"})
            .values({expected_rows.default_value()})
            .values({expected_rows.value("explicit")})
            .values({expected_rows.default_value()});
        for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
            const auto expected = batch.query().compile(driver, db_parameter_mode::literal);
            const auto before = appended.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(before.sql(), expected.sql());
            const auto positioned = expected_rows.compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(before.sql(), positioned.sql());
            EXPECT_TRUE(testing::throws_on([&] { appended.values(std::span<const entity_type>{}); }));
            const auto after = appended.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(after.sql(), before.sql());
            const auto parameterized = appended.query().compile(driver);
            EXPECT_EQ(parameterized.params().size(), std::size_t{1});
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_insert_sources_cannot_replace_or_append_value_rows) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<entity_type>();
        entity_type entity;
        entity.set<"name">("retained");
        auto source = repository.query_builder();
        source.select({"id"});
        db_query raw_source;
        raw_source.select(raw_source.value(8));
        auto values = repository.insert_builder(entity);
        auto defaults = repository.insert_builder(entity_type{});
        auto selected = repository.insert_builder();
        selected.insert_from({"id"}, source);
        for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
            const auto before = values.query().compile(driver, db_parameter_mode::literal);
            const auto default_before = defaults.query().compile(driver, db_parameter_mode::literal);
            const auto selected_before = selected.query().compile(driver, db_parameter_mode::literal);
            EXPECT_TRUE(testing::throws_on([&] { values.insert_from({"id"}, source); }));
            EXPECT_TRUE(testing::throws_on([&] { values.insert_from({"name"}, raw_source); }));
            EXPECT_TRUE(testing::throws_on([&] { defaults.insert_from({"id"}, raw_source); }));
            EXPECT_TRUE(testing::throws_on([&] { selected.values(entity); }));
            EXPECT_TRUE(testing::throws_on([&] { selected.values(entity_type{}); }));
            const auto after = values.query().compile(driver, db_parameter_mode::literal);
            const auto default_after = defaults.query().compile(driver, db_parameter_mode::literal);
            const auto selected_after = selected.query().compile(driver, db_parameter_mode::literal);
            EXPECT_EQ(after.sql(), before.sql());
            EXPECT_EQ(default_after.sql(), default_before.sql());
            EXPECT_EQ(selected_after.sql(), selected_before.sql());
        }
        const borrowed_text keys[]{"id"};
        const auto driver = database_config().driver_;
        values.on_conflict(driver == db_driver::postgresql
                               ? db_upsert_options{.conflict_paths_ = keys}
                               : db_upsert_options{.any_unique_key_ = true});
        const auto upsert = values.get_query_and_parameters();
        EXPECT_TRUE(upsert.sql().find(driver == db_driver::postgresql
                                          ? "\"name\" = excluded.\"name\""
                                          : "`name` = VALUES(`name`)") != std::string_view::npos);
        co_return;
    });
}

TEST(Ruvia, db_repository_batch_set_rejects_invalid_suffix_without_publishing_prefix) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, database_config(), [&](db_client& client) -> task<void> {
        auto repository = client.db().repository<computed_entity_type>();
        auto update = repository.update_builder("i");
        update.set("name", "original").where(computed_entity_type::column<"id">() == 7);
        const std::array invalid{
            db_assignment{"unknown", db_expr::value(9)},
            db_assignment{"name_length", db_expr::value(9)},
            db_assignment{"name", db_expr{}},
        };
        for (const auto& rejected : invalid) {
            for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
                const auto before = update.query().compile(driver, db_parameter_mode::literal);
                const auto bound_before = update.query().compile(driver);
                const db_assignment assignments[]{
                    {"name", db_expr::value("not committed")},
                    {"id", db_expr::value(99)},
                    rejected,
                };
                EXPECT_TRUE(testing::throws_on([&] { update.set(std::span<const db_assignment>(assignments)); }));
                const auto after = update.query().compile(driver, db_parameter_mode::literal);
                const auto bound_after = update.query().compile(driver);
                EXPECT_EQ(after.sql(), before.sql());
                EXPECT_EQ(bound_after.sql(), bound_before.sql());
                EXPECT_EQ(bound_after.params().size(), bound_before.params().size());
            }
        }
        db_query expressions;
        update.set({{"name", expressions.value("committed")}, {"id", expressions.value(8)}});
        computed_entity_type changes;
        changes.set<"name">("entity");
        changes.set<"id">(10);
        update.set(changes);
        for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
            const auto statement = update.query().compile(driver, db_parameter_mode::literal);
            const auto expected_value = db_query::render_expression(expressions.value("entity"), driver);
            const auto expected_assignment = std::string(driver == db_driver::postgresql ? "\"name\" = " : "`name` = ") + std::string(expected_value);
            EXPECT_TRUE(statement.sql().find(expected_assignment) != std::string_view::npos);
            EXPECT_TRUE(statement.sql().find(driver == db_driver::postgresql ? "\"id\" = 10" : "`id` = 10") != std::string_view::npos);
        }
        co_return;
    });
}

TEST(Ruvia, db_repository_upsert_option_families_validate_paths_and_driver_rules) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        repository_runtime runtime;
        test::with_connected_db_client(runtime.attachment_, db_config{.driver_ = driver}, [&](db_client& client) -> task<void> {
            auto repository = client.db().repository<entity_type>();
            entity_type input;
            input.set<"id">(1);
            input.set<"name">("Ada");

            const std::array<borrowed_text, 1> conflict_paths{"id"};
            const std::array<borrowed_text, 1> update_columns{"name"};
            db_upsert_options options;
            if (driver == db_driver::postgresql) {
                options.conflict_paths_ = conflict_paths;
                options.update_columns_ = update_columns;
                options.skip_update_if_no_values_changed_ = true;
                options.index_predicate_ = entity_type::column<"name">().is_not_null();
                auto conditional = repository.upsert(input, options);
                (void)conditional;

                options = {};
                options.conflict_paths_ = conflict_paths;
                options.do_nothing_ = true;
                auto nothing = repository.upsert(input, options);
                (void)nothing;
            } else {
                options.any_unique_key_ = true;
                auto any_key = repository.upsert(input, options);
                (void)any_key;
                EXPECT_TRUE(testing::throws_on([&] {
                    (void)repository.upsert(input, db_upsert_options{.do_nothing_ = true, .any_unique_key_ = true});
                }));
                EXPECT_TRUE(testing::throws_on([&] {
                    (void)repository.upsert(input, db_upsert_options{.conflict_paths_ = conflict_paths});
                }));
                EXPECT_TRUE(testing::throws_on([&] {
                    (void)repository.upsert(input, db_upsert_options{.any_unique_key_ = true, .skip_update_if_no_values_changed_ = true});
                }));
            }
            EXPECT_TRUE(testing::throws_on([&] {
                const borrowed_text missing_columns[]{"missing"};
                (void)repository.upsert(input, db_upsert_options{.conflict_paths_ = missing_columns});
            }));
            EXPECT_TRUE(testing::throws_on([&] {
                const borrowed_text missing_columns[]{"missing"};
                (void)repository.upsert(input, db_upsert_options{.update_columns_ = missing_columns});
            }));
            co_return;
        });
    }
}

TEST(Ruvia, db_repository_save_compiles_returning_on_every_driver_but_update_returning_only_on_postgresql) {
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        repository_runtime runtime;
        test::with_connected_db_client(runtime.attachment_, db_config{.driver_ = driver}, [&](db_client& client) -> task<void> {
            auto repository = client.db().repository<entity_type>();
            entity_type input;
            input.set<"id">(1);
            input.set<"name">("Ada");
            // save() upserts through RETURNING; MariaDB accepts it on INSERT.
            auto saved = repository.save(input);
            (void)saved;
            std::array<entity_type, 2> batch;
            batch[0].set<"id">(1);
            batch[0].set<"name">("Ada");
            batch[1].set<"id">(2);
            batch[1].set<"name">("Bob");
            auto saved_many = repository.save(batch);
            (void)saved_many;

            auto updated = repository.update_builder();
            updated.where(entity_type::column<"id">() == 1).set({entity_type::column<"name">() = "next"}).returning({{"id"}});
            if (driver == db_driver::postgresql) {
                auto one = updated.get_one();
                (void)one;
            } else {
                EXPECT_TRUE(testing::throws_on([&] { (void)updated.get_one(); }));
            }
            co_return;
        });
    }
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_DB_ENTITY(role_entity, "roles",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string>)
RUVIA_DB_ENTITY(user_role_entity, "user_roles",
    db_column<"user_id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"role_id", std::int64_t, db_column_options{.primary_key_ = true}>)
RUVIA_DB_ENTITY(trigger_populated_entity, "devices",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"name", std::string, db_column_options{.nullable_ = true}>,
    db_column<"revision", std::int64_t>,
    db_column<"protocol_address", std::string, db_column_options{.read_only_ = true}>,
    db_column<"updated_at", std::string>)

TEST(Ruvia, db_repository_builders_support_typed_joins_assignments_upserts_delete_using_and_read_only_columns) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto roles = client.db().repository<role_entity>();
        auto query = roles.query_builder("r");
        query.join<user_role_entity>(db_join_type::inner, "ur", user_role_entity::column<"role_id">() == role_entity::column<"id">())
            .where(user_role_entity::column<"user_id">("ur") == 42)
            .distinct_on({"id"})
            .order_by(role_entity::column<"id">())
            .add_order_by(user_role_entity::column<"role_id">("ur"), db_order_direction::desc);
        const auto role_sql = query.get_query_and_parameters();
        EXPECT_EQ(role_sql.sql(),
            "SELECT DISTINCT ON (\"r\".\"id\") \"r\".\"id\", \"r\".\"name\" FROM \"roles\" AS \"r\" "
            "INNER JOIN \"user_roles\" AS \"ur\" ON (\"ur\".\"role_id\" = \"r\".\"id\") "
            "WHERE (\"ur\".\"user_id\" = $1) ORDER BY \"r\".\"id\" ASC, \"ur\".\"role_id\" DESC");

        auto clean_grouped = roles.query_builder("r");
        clean_grouped.select({{"id"}, role_entity::column<"name">().coalesce("fallback", "label")})
            .group_by({"id"})
            .add_group_by({role_entity::column<"name">()})
            .having(role_entity::column<"id">() > 0)
            .and_having(query.exists());
        const auto grouped_sql = clean_grouped.get_query_and_parameters();
        EXPECT_TRUE(grouped_sql.sql().find("GROUP BY \"r\".\"id\", \"r\".\"name\" HAVING ((\"r\".\"id\" > $2) AND EXISTS (SELECT") != std::string_view::npos);

        auto devices = client.db().repository<trigger_populated_entity>();
        trigger_populated_entity dev;
        dev.set<"id">(1);
        dev.set<"name">("sensor-1");
        dev.set<"revision">(1);
        dev.set<"protocol_address">("ignored-by-orm-writes");
        dev.set<"updated_at">("2026-01-01T00:00:00Z");

        auto insert_dev = devices.insert_builder(dev);
        const borrowed_text conflict_paths[]{"id"};
        insert_dev.on_conflict({.conflict_paths_ = conflict_paths, .skip_update_if_no_values_changed_ = true}).returning();
        const auto insert_dev_sql = insert_dev.get_query_and_parameters();
        EXPECT_TRUE(insert_dev_sql.sql().find("protocol_address\") VALUES") == std::string_view::npos);
        EXPECT_TRUE(insert_dev_sql.sql().find("DO UPDATE SET \"name\" = excluded.\"name\"") != std::string_view::npos);
        EXPECT_TRUE(insert_dev_sql.sql().find("RETURNING \"devices\".\"id\", \"devices\".\"name\", \"devices\".\"revision\", \"devices\".\"protocol_address\", \"devices\".\"updated_at\"") != std::string_view::npos);
        auto one_inserted = insert_dev.get_one();
        (void)one_inserted;

        EXPECT_TRUE(testing::throws_on([&] {
            auto bad = devices.update_builder();
            bad.set("protocol_address", "x");
        }));

        db_query incoming;
        incoming.values({incoming.value(10), incoming.value("edge-a"), incoming.value(1), incoming.value("now")});
        auto from_cte = devices.query_builder("inc");
        from_cte.from_cte("incoming")
            .select({
                {"id", incoming.column("id", "inc")},
                {"name", incoming.column("name", "inc")},
                {"revision", incoming.column("revision", "inc")},
                {"updated_at", incoming.column("updated_at", "inc")},
            });
        auto pipeline = devices.insert_builder();
        const borrowed_text cte_columns[]{"id", "name", "revision", "updated_at"};
        const db_assignment update_expressions[]{trigger_populated_entity::column<"revision">().increment(1)};
        pipeline.with("incoming", incoming, {.columns_ = cte_columns})
            .insert_from({"id", "name", "revision", "updated_at"}, from_cte)
            .on_conflict({
                .conflict_paths_ = conflict_paths,
                .skip_update_if_no_values_changed_ = true,
                .update_expressions_ = update_expressions,
                .update_where_ = trigger_populated_entity::column<"revision">() < 100,
            })
            .returning({{"id"}});
        const auto pipeline_sql = pipeline.get_query_and_parameters();
        EXPECT_TRUE(pipeline_sql.sql().find("WITH \"incoming\" (\"id\", \"name\", \"revision\", \"updated_at\") AS (VALUES ($1, $2, $3, $4))") == 0);
        EXPECT_TRUE(pipeline_sql.sql().find("ON CONFLICT (\"id\") DO UPDATE SET") != std::string_view::npos);
        EXPECT_TRUE(pipeline_sql.sql().find("\"revision\" = (\"devices\".\"revision\" + $5)") != std::string_view::npos);

        auto update_builder = devices.update_builder("d");
        update_builder.set("name", "renamed")
            .set_null("name")
            .increment("revision", 2)
            .set_now("updated_at")
            .update_from<role_entity>("r")
            .join<user_role_entity>(db_join_type::inner, "ur", user_role_entity::column<"role_id">("ur") == role_entity::column<"id">("r"))
            .where(trigger_populated_entity::column<"id">() == user_role_entity::column<"user_id">("ur"))
            .returning({{"id"}});
        const auto update_sql = update_builder.get_query_and_parameters();
        EXPECT_EQ(update_sql.sql(),
            "UPDATE \"devices\" AS \"d\" SET \"name\" = NULL, \"revision\" = (\"d\".\"revision\" + $1), \"updated_at\" = \"now\"() "
            "FROM \"roles\" AS \"r\" INNER JOIN \"user_roles\" AS \"ur\" ON (\"ur\".\"role_id\" = \"r\".\"id\") "
            "WHERE (\"d\".\"id\" = \"ur\".\"user_id\") RETURNING \"d\".\"id\" AS \"id\"");

        auto delete_builder = devices.delete_builder("d");
        delete_builder.delete_using<role_entity>("r")
            .join_cte(db_join_type::inner, "expired", "e", role_entity::column<"id">("r") == role_entity::column<"id">("e"))
            .where(trigger_populated_entity::column<"id">() == role_entity::column<"id">("r"))
            .returning({{"id"}});
        const auto delete_sql = delete_builder.get_query_and_parameters();
        EXPECT_EQ(delete_sql.sql(),
            "DELETE FROM \"devices\" AS \"d\" USING \"roles\" AS \"r\" INNER JOIN \"expired\" AS \"e\" ON (\"r\".\"id\" = \"e\".\"id\") "
            "WHERE (\"d\".\"id\" = \"r\".\"id\") RETURNING \"d\".\"id\" AS \"id\"");

        auto ret_one_builder = devices.update_builder();
        ret_one_builder.where(trigger_populated_entity::column<"id">() == 1)
            .set({
                trigger_populated_entity::column<"name">() = "next",
                trigger_populated_entity::column<"revision">() += 1,
                trigger_populated_entity::column<"updated_at">().set_now(),
            })
            .returning({{"id"}, trigger_populated_entity::column<"name">().coalesce("", "label"), {"rank", trigger_populated_entity::column<"revision">()}});
        auto ret_one = ret_one_builder.get_one<item_summary>();
        (void)ret_one;
        co_return;
    });
}

RUVIA_DB_ENTITY(soft_entity, "articles",
    db_column<"id", std::int64_t, db_column_options{.primary_key_ = true}>,
    db_column<"title", std::string>,
    db_column<"views", std::int64_t>,
    db_column<"revision", std::int64_t, db_column_options{.version_ = true}>,
    db_column<"updated_at", std::string, db_column_options{.update_date_ = true}>,
    db_column<"deleted_at", std::string, db_column_options{.delete_date_ = true}>)

TEST(Ruvia, db_repository_typeorm_style_api_covers_soft_delete_save_aggregates_joins_and_raw_queries) {
    repository_runtime runtime;
    test::with_connected_db_client(runtime.attachment_, {.driver_ = db_driver::postgresql}, [&](db_client& client) -> task<void> {
        auto repo = client.db().repository<soft_entity>();

        auto active_qb = repo.query_builder("a");
        active_qb.select({"id", "title"})
            .add_select("views")
            .add_select(db_expr::count(), "total_count")
            .where(soft_entity::column<"views">() >= 10)
            .and_where(soft_entity::column<"title">().ilike("%ruvia%") || (soft_entity::column<"id">() == 1))
            .group_by({"id", "title", "views"})
            .having(soft_entity::column<"views">() > 0)
            .or_having(soft_entity::column<"id">() == 1)
            .limit(20)
            .offset(5);
        const auto active_sql = active_qb.get_query_and_parameters();
        EXPECT_TRUE(active_sql.sql().find("SELECT \"a\".\"id\" AS \"id\", \"a\".\"title\" AS \"title\", \"a\".\"views\" AS \"views\", \"count\"(*) AS \"total_count\" FROM \"articles\" AS \"a\"") == 0);
        EXPECT_TRUE(active_sql.sql().find("(\"a\".\"deleted_at\" IS NULL)") != std::string_view::npos);
        EXPECT_TRUE(active_sql.sql().find("HAVING ((\"a\".\"views\" > $4) OR (\"a\".\"id\" = $5))") != std::string_view::npos);
        EXPECT_TRUE(active_sql.sql().find("LIMIT $6 OFFSET $7") != std::string_view::npos);
        const auto borrowed = active_qb.query().compile(db_driver::postgresql);
        EXPECT_TRUE(borrowed.sql().find("deleted_at") == std::string_view::npos);

        active_qb.with_deleted();
        const auto with_deleted_sql = active_qb.get_query_and_parameters();
        EXPECT_TRUE(with_deleted_sql.sql().find("deleted_at") == std::string_view::npos);
        EXPECT_EQ(with_deleted_sql.sql(), borrowed.sql());

        auto parents = client.db().repository<parent>();
        auto rel_join = parents.query_builder("p");
        rel_join.left_join("children", "c", child::column<"id">("c") > 0)
            .add_select(db_expr::count(child::column<"id">("c")), "child_count")
            .group_by({"id"});
        const auto rel_join_sql = rel_join.get_query_and_parameters();
        EXPECT_EQ(rel_join_sql.sql(),
            "SELECT \"p\".\"id\", \"count\"(\"c\".\"id\") AS \"child_count\" FROM \"parents\" AS \"p\" "
            "LEFT JOIN \"children\" AS \"c\" ON ((\"p\".\"id\" = \"c\".\"parent_id\") AND (\"c\".\"id\" > $1)) "
            "GROUP BY \"p\".\"id\"");

        db_values_builder incoming({"id", "title", "views"});
        incoming.row({db_expr::value(1), db_expr::value("Hello"), db_expr::value(10)});
        auto values_select = repo.query_builder("v");
        values_select.with_deleted()
            .with("incoming", incoming, db_materialization::materialized)
            .from_cte("incoming")
            .select({"id", "title", "views"});
        const auto values_sql = values_select.get_query_and_parameters();
        EXPECT_TRUE(values_sql.sql().find("WITH \"incoming\" (\"id\", \"title\", \"views\") AS MATERIALIZED (VALUES ($1, $2, $3))") == 0);

        soft_entity item;
        item.set<"id">(1);
        item.set<"title">("Ruvia ORM");
        item.set<"views">(100);
        auto ins_ignore = repo.insert_builder();
        ins_ignore.values(item).on_conflict({.do_nothing_ = true}).returning({"id"});
        const auto ins_ignore_sql = ins_ignore.get_query_and_parameters();
        EXPECT_EQ(ins_ignore_sql.sql(),
            "INSERT INTO \"articles\" (\"id\", \"title\", \"views\") VALUES ($1, $2, $3) ON CONFLICT DO NOTHING RETURNING \"articles\".\"id\" AS \"id\"");

        auto ins_update = repo.insert_builder();
        const borrowed_text conflict_paths[]{"id"};
        const borrowed_text update_columns[]{"title", "views"};
        ins_update.values(item).on_conflict({.conflict_paths_ = conflict_paths, .update_columns_ = update_columns}).returning({"id"});
        const auto ins_update_sql = ins_update.get_query_and_parameters();
        // The conflict update stamps the lifecycle columns the caller left unnamed.
        EXPECT_EQ(ins_update_sql.sql(),
            "INSERT INTO \"articles\" (\"id\", \"title\", \"views\") VALUES ($1, $2, $3) ON CONFLICT (\"id\") DO UPDATE SET \"title\" = excluded.\"title\", \"views\" = excluded.\"views\", "
            "\"updated_at\" = \"now\"(), \"revision\" = (\"articles\".\"revision\" + $4) RETURNING \"articles\".\"id\" AS \"id\"");
        {
            auto op1 = repo.find({.where_ = soft_entity::column<"id">() == 1});
            auto op2 = repo.find_one({.where_ = soft_entity::column<"id">() == 1});
            auto op3 = repo.find_and_count({.where_ = soft_entity::column<"id">() == 1});
            auto op4 = repo.count({.where_ = soft_entity::column<"id">() == 1});
            auto op5 = repo.exists({.where_ = soft_entity::column<"id">() == 1});
            auto op6 = repo.save(item);
            auto op7 = repo.soft_delete(soft_entity::column<"id">() == 1);
            auto op8 = repo.restore(soft_entity::column<"id">() == 1);
            auto op13 = repo.clear();
            auto op15 = active_qb.get_raw_many();
            auto op16 = rel_join.get_raw_and_entities();
            auto op17 = active_qb.get_scalar<std::int64_t>();
            auto op18 = active_qb.get_scalars<std::int64_t>();
            auto op19 = ins_update.get_scalar<std::int64_t>();
            (void)op1;
            (void)op2;
            (void)op3;
            (void)op4;
            (void)op5;
            (void)op6;
            (void)op7;
            (void)op8;
            (void)op13;
            (void)op15;
            (void)op16;
            (void)op17;
            (void)op18;
            (void)op19;
        }
        co_return;
    });
}
#endif
}  // namespace db_repository_test

#ifdef RUVIA_ENABLE_POSTGRESQL
TEST(Ruvia, db_postgresql_declares_float8_for_doubles_and_leaves_other_parameters_untyped) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("raw")})}, {.rows_ = std::vector<std::string>({std::string("compiled")})}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        // An integral double must still reach the server as float8.
        const std::array<db_value, 4> params{db_value{5}, db_value{5.0}, db_value{true}, db_value{"text"}};
        (void)co_await handle.query("SELECT $1, $2, $3, $4", std::span<const db_value>(params));
        // Untyped, json_build_object's variadic "any" arguments cannot be resolved.
        db_query query;
        query.select(query.call("json_build_object", {query.value("k"), query.value(5)})).from("records");
        const auto compiled = query.compile(db_driver::postgresql);
        (void)co_await handle.query(compiled.sql(), compiled.params());
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{2});
    if (statements.size() == 2) {
        // The SQL decides every type except a double's, which is declared
        // float8 regardless of its value; compiled SQL casts from the value category.
        EXPECT_TRUE(statements[0].sql_ == "SELECT $1, $2, $3, $4");
        EXPECT_TRUE((statements[0].parameter_types_ == std::vector<std::uint32_t>({0, 701, 0, 0})));
        EXPECT_TRUE(statements[1].sql_ == "SELECT \"json_build_object\"($1::text, $2::int8) FROM \"records\"");
        EXPECT_TRUE((statements[1].parameter_types_ == std::vector<std::uint32_t>({0, 0})));
        EXPECT_TRUE((statements[0].parameter_values_ ==
                     std::vector<std::optional<std::string>>({std::string("5"), std::string("5"), std::string("true"), std::string("text")})));
        EXPECT_TRUE((statements[1].parameter_values_ ==
                     std::vector<std::optional<std::string>>({std::string("k"), std::string("5")})));
    }
}

TEST(Ruvia, db_postgresql_owns_lazy_query_inputs_and_preserves_text_parameter_values) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")})}, {.rows_ = std::vector<std::string>({std::string("second")})}}));
    auto& io = test::new_test_io_context();
    auto attachment = attach_event_loop(io);
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto operation = [&] {
            const std::array<char, 4> unterminated{'a', 'b', 'c', '!'};
            const std::string long_value(4096, 'x');
            const std::array params{db_value{std::string_view(unterminated.data(), 3)}, db_value{long_value},
                db_value{std::string_view{}}, db_value{nullptr}, db_value{false}, db_value{-7},
                db_value{std::uint64_t{18446744073709551615ULL}}, db_value{1.5}};
            return handle.query("SELECT $1, $2, $3, $4, $5, $6, $7, $8", params);
        }();
        (void)co_await std::move(operation);
        // A compiled value is checked by the driver before it reaches the wire.
        const std::string invalid_text("a\0b", 3);
        const std::array invalid_params{db_value{std::string_view(invalid_text)}};
        bool rejected = false;
        try {
            (void)co_await handle.query("SELECT $1", invalid_params);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        // Validation failure keeps the connection reusable, and this query
        // must not borrow the previous operation's parameter storage.
        const std::array params{db_value{"next"}, db_value{9}};
        (void)co_await handle.query("SELECT $1, $2", params);
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{2});
    if (statements.size() == 2) {
        EXPECT_TRUE((statements[0].parameter_values_ ==
                     std::vector<std::optional<std::string>>({std::string("abc"), std::string(4096, 'x'), std::string(""), std::nullopt,
                         std::string("false"), std::string("-7"), std::string("18446744073709551615"), std::string("1.5")})));
        EXPECT_TRUE((statements[1].parameter_values_ == std::vector<std::optional<std::string>>({std::string("next"), std::string("9")})));
    }
}

namespace db_repository_array_test {

RUVIA_DB_ENTITY(array_entity, "array_records", ruvia::db_column<"values", std::vector<int>>)

struct array_case final {
    std::string_view text_;
    std::vector<int> values_{};
};

TEST(Ruvia, db_repository_decodes_postgresql_array_bounds_in_element_order) {
    using namespace ruvia;
    const std::array valid{
        array_case{"{7,8,9}", std::vector<int>({7, 8, 9})},
        array_case{"[0:2]={7,8,9}", std::vector<int>({7, 8, 9})},
        array_case{"[0:0]={7}", std::vector<int>({7})},
        array_case{"[-3:-1]={9,7,8}", std::vector<int>({9, 7, 8})},
        array_case{"[2:4]={8,9,7}", std::vector<int>({8, 9, 7})},
        array_case{"[-2147483648:-2147483647]={7,8}", std::vector<int>({7, 8})},
        array_case{"[2147483645:2147483646]={8,9}", std::vector<int>({8, 9})},
        array_case{"{}", std::vector<int>()},
    };
    const std::array invalid{
        "[0:2]={7,8}",
        "[-3:-1]={7,8,9,10}",
        "[2:1]={7}",
        "[0:1]={}",
        "[-2147483649:-2147483648]={7,8}",
        "[2147483647:2147483648]={7,8}",
        "[2147483647:2147483647]={7}",
        "[-2147483648:2147483647]={7}",
        "[-2147483648:-1]={7}",
        "[:1]={7}",
        "[0:]={7}",
        "[0:0]{7}",
        "[0:0]x={7}",
        "[0:1][0:1]={{7,8},{9,10}}",
        "{{7,8},{9,10}}",
        "[1:3]={7}",
        "[0:2147483646]={7}",
    };
    std::vector<test::backend_startup_peer::postgres_reply> replies{};
    for (const auto& value : valid) {
        replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {std::string(value.text_)}});
    }
    for (const auto value : invalid) {
        replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {value}});
    }
    replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {"{21,22}"}});
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<array_entity>();
        for (const auto& value : valid) {
            bool decoded = false;
            try {
                const auto rows = co_await repository.find();
                EXPECT_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    EXPECT_TRUE(std::ranges::equal(rows[0].get<"values">(), value.values_));
                }
                decoded = true;
            } catch (const db_conversion_error&) {
                decoded = false;
            }
            EXPECT_TRUE(decoded);
        }
        for ([[maybe_unused]] const auto value : invalid) {
            bool rejected = false;
            try {
                (void)co_await repository.find();
            } catch (const db_conversion_error& error) {
                rejected = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(rejected);
        }
        const auto rows = co_await repository.find();
        EXPECT_EQ(rows.size(), std::size_t{1});
        if (!rows.empty()) {
            EXPECT_TRUE(std::ranges::equal(rows[0].get<"values">(), std::array{21, 22}));
        }
    });
}

TEST(Ruvia, db_repository_bounds_declared_postgresql_array_length_by_arrived_text) {
    using namespace ruvia;
    std::vector<test::backend_startup_peer::postgres_reply> replies{};
    replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {"[0:2147483646]={7}"}});
    replies.push_back({.column_ = "values", .type_ = 1007, .rows_ = {"{21,22}"}});
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<array_entity>();
        bool rejected = false;
        try {
            (void)co_await repository.query_builder().get_scalar<std::vector<int>>();
        } catch (const db_conversion_error& error) {
            rejected = error.code() == db_conversion_error::code_type::invalid_format;
        }
        EXPECT_TRUE(rejected);
        const auto values = co_await repository.query_builder().get_scalar<std::vector<int>>();
        EXPECT_TRUE(values.has_value());
        if (values) {
            EXPECT_TRUE(std::ranges::equal(*values, std::array{21, 22}));
        }
    });
}

}  // namespace db_repository_array_test

namespace db_repository_lifecycle_test {

RUVIA_DB_ENTITY(versioned_entity, "articles",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"views", std::int64_t>,
    ruvia::db_column<"revision", std::int64_t, ruvia::db_column_options{.version_ = true}>,
    ruvia::db_column<"updated_at", std::string, ruvia::db_column_options{.update_date_ = true}>,
    ruvia::db_column<"deleted_at", std::string, ruvia::db_column_options{.delete_date_ = true}>)

// Every repository write that changes a row moves the version token and the
// update date, unless the caller assigns that column itself.
TEST(Ruvia, db_repository_write_paths_stamp_version_and_update_date) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    std::vector<peer_type::postgres_reply> replies(6, peer_type::postgres_reply{.column_ = std::string(""), .command_ = std::string("UPDATE 1")});
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<versioned_entity>();
        const auto row = versioned_entity::column<"id">() == 1;
        (void)co_await repository.restore(row);
        (void)co_await repository.soft_delete(row);
        (void)co_await repository.increment(row, "views");
        const std::array assignments{versioned_entity::column<"views">().set(5)};
        (void)co_await repository.update(row, std::span<const db_assignment>(assignments));
        versioned_entity entity;
        entity.set<"id">(1);
        entity.set<"views">(7);
        const borrowed_text conflict_paths[]{"id"};
        (void)co_await repository.upsert(entity, {.conflict_paths_ = conflict_paths});
        versioned_entity explicit_version;
        explicit_version.set<"revision">(42);
        (void)co_await repository.update(row, explicit_version);
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{6});
    for (std::size_t i = 0; i < statements.size(); ++i) {
        const std::string_view sql = statements[i].sql_;
        EXPECT_TRUE(sql.find("\"updated_at\" = \"now\"()") != std::string_view::npos);
        const bool bumped = sql.find("\"revision\" = (\"articles\".\"revision\" + $") != std::string_view::npos;
        EXPECT_EQ(bumped, i + 1 < statements.size());
    }
    if (statements.size() == 6) {
        EXPECT_TRUE(std::string_view(statements[5].sql_).find("\"revision\" = $") != std::string_view::npos);
        EXPECT_TRUE(std::ranges::find(statements[5].parameter_values_, std::optional<std::string>{std::in_place, "42"}) != statements[5].parameter_values_.end());
    }
}

}  // namespace db_repository_lifecycle_test

namespace db_repository_result_column_test {

RUVIA_DB_ENTITY(named_entity, "named_records",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"name", std::string>)

// A mapped column must resolve to exactly one result column in every
// projection mode; a repeated name is ambiguous rather than "first wins".
TEST(Ruvia, db_repository_rejects_repeated_mapped_result_columns_without_wildcards) {
    using namespace ruvia;
    using peer_type = test::backend_startup_peer;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.columns_ = std::vector<std::string>({std::string("id"), std::string("name"), std::string("name")}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({std::string("1"), std::string("first"), std::string("second")})})},
            {.columns_ = std::vector<std::string>({std::string("name"), std::string("id")}), .values_ = std::vector<std::vector<std::string>>({std::vector<std::string>({std::string("third"), std::string("3")})})}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto repository = connected.db().repository<named_entity>();
        bool ambiguous = false;
        try {
            (void)co_await repository.find();
        } catch (const db_conversion_error& error) {
            ambiguous = error.code() == db_conversion_error::code_type::invalid_format;
        }
        EXPECT_TRUE(ambiguous);
        const auto rows = co_await repository.find();
        EXPECT_EQ(rows.size(), std::size_t{1});
        if (!rows.empty()) {
            EXPECT_EQ(rows[0].get<"id">(), std::int64_t{3});
            EXPECT_EQ(rows[0].get<"name">(), "third");
        }
    });
}

}  // namespace db_repository_result_column_test
#endif

namespace {

template <typename columns_type>
concept db_find_options_accepts_select = requires(columns_type&& columns) {
    ruvia::db_find_options{.select_ = std::forward<columns_type>(columns)};
};

template <typename columns_type>
concept db_upsert_options_accepts_conflict_paths = requires(columns_type&& columns) {
    ruvia::db_upsert_options{.conflict_paths_ = std::forward<columns_type>(columns)};
};

static_assert(db_find_options_accepts_select<const std::vector<ruvia::borrowed_text>&>);
static_assert(!db_find_options_accepts_select<std::vector<ruvia::borrowed_text>>);
static_assert(!db_find_options_accepts_select<std::array<ruvia::borrowed_text, 1>>);
static_assert(db_upsert_options_accepts_conflict_paths<const std::vector<ruvia::borrowed_text>&>);
static_assert(!db_upsert_options_accepts_conflict_paths<std::vector<ruvia::borrowed_text>>);
static_assert(!db_upsert_options_accepts_conflict_paths<std::array<ruvia::borrowed_text, 1>>);

template <typename columns_type>
concept db_conflict_options_accepts_columns = requires(columns_type&& columns) {
    ruvia::db_conflict_options{.columns_ = std::forward<columns_type>(columns)};
};

template <typename partition_type>
concept db_window_options_accepts_partition = requires(partition_type&& partitions) {
    ruvia::db_window_options{.partition_by_ = std::forward<partition_type>(partitions)};
};

static_assert(db_conflict_options_accepts_columns<const std::vector<ruvia::borrowed_text>&>);
static_assert(!db_conflict_options_accepts_columns<std::vector<ruvia::borrowed_text>>);
static_assert(!db_conflict_options_accepts_columns<std::array<ruvia::borrowed_text, 1>>);
static_assert(db_window_options_accepts_partition<const std::vector<ruvia::db_expr>&>);
static_assert(!db_window_options_accepts_partition<std::vector<ruvia::db_expr>>);
static_assert(!db_window_options_accepts_partition<std::array<ruvia::db_expr, 1>>);
}  // namespace
