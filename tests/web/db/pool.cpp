#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/event_loop_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_schema.h"

#include "backend_client_fixture.h"
#include "test_io_context.h"

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
            false, peer_type::after_startup::idle, std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>(ruvia::process_resource()), 3);
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
            false, peer_type::after_startup::idle, std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>(ruvia::process_resource()), 2);
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
            std::pmr::vector<peer_type::maria_reply> replies({{.column_ = std::pmr::string("", ruvia::process_resource()), .status_ = 3}}, ruvia::process_resource());
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("BEGIN", ruvia::process_resource()), .status_ = 'T'},
                                                                                {.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("BEGIN", ruvia::process_resource()), .status_ = 'T'},
                                                                                {.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("ROLLBACK", ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("available", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("ROLLBACK", ruvia::process_resource())}},
            ruvia::process_resource()),
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("BEGIN", ruvia::process_resource()), .status_ = 'T'},
                                                                                {.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("BEGIN", ruvia::process_resource()), .status_ = 'T'}},
            ruvia::process_resource()),
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
