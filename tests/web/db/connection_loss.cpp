#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

enum class statement_kind { query,
    execute,
    stream };

ruvia::task<void> run_statement(ruvia::db_handle& handle, statement_kind kind) {
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
    ruvia::testing::test_context& ruvia_ctx, ruvia::test::backend_startup_peer& peer, ruvia::db_driver driver) {
    using namespace ruvia;
    auto config = peer.config();
    config.driver_ = driver;
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config);
    std::string before;
    std::optional<db_error::code_type> code;
    std::string after;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
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
    RUVIA_CHECK(before == "before");
    RUVIA_CHECK(!code.has_value());
    RUVIA_CHECK(after == "after");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

}  // namespace

// The peer drops the connection after the first row of a row set. Every
// statement shape must report that as an I/O failure: neither as caller misuse
// ("requires row-producing SQL"), nor -- for execute() -- as success.
RUVIA_TEST(db_connection_lost_mid_result_is_an_io_error) {
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
                    auto handle = client.handle();
                    try {
                        co_await run_statement(handle, kind);
                    } catch (const db_error& error) {
                        code = error.code();
                    }
                });
            RUVIA_CHECK(code == db_error::code_type::io_error);
        }
    }
}

// The server ends a pooled session while the pool holds it idle (PostgreSQL
// idle_session_timeout or pg_terminate_backend, MariaDB wait_timeout or KILL).
// The next lease looks at the connection before sending anything, reconnects,
// and the statement succeeds on the new session.
RUVIA_TEST(db_session_ended_while_idle_reconnects_before_the_next_statement) {
    using peer_type = ruvia::test::backend_startup_peer;
#ifdef RUVIA_ENABLE_POSTGRESQL
    {
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
            {{.rows_ = {"before"}, .close_after_ = true}, {.rows_ = {"after"}}});
        check_session_ended_while_idle(ruvia_ctx, peer, ruvia::db_driver::postgresql);
    }
#endif
#ifdef RUVIA_ENABLE_MARIADB
    {
        peer_type peer(std::vector<peer_type::maria_reply>{{.rows_ = {"before"}, .close_after_ = true}, {.rows_ = {"after"}}},
            peer_type::after_startup::reaccept);
        check_session_ended_while_idle(ruvia_ctx, peer, ruvia::db_driver::mariadb);
    }
#endif
}

#ifdef RUVIA_ENABLE_POSTGRESQL
// A statement the server rejected leaves the connection in sync. The peer
// accepts one connection, so the next statement succeeding proves the pool
// kept it rather than reconnecting.
RUVIA_TEST(db_rejected_statement_keeps_the_pooled_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.error_ = "duplicate key value violates unique constraint", .sql_state_ = "23505"},
            {.rows_ = {"kept"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::string sql_state;
    std::string kept;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        try {
            (void)co_await handle.execute("INSERT INTO jobs (key) VALUES ('a')");
        } catch (const db_error& error) {
            code = error.code();
            sql_state = error.sql_state().value_or("");
        }
        const auto rows = co_await handle.query("SELECT v FROM records");
        kept = rows.front()["v"].as<std::string_view>().value_or("");
    });
    RUVIA_CHECK(code == db_error::code_type::statement_failed);
    RUVIA_CHECK(sql_state == "23505");
    RUVIA_CHECK(kept == "kept");
    RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{2});
}

// A statement that fails inside a transaction fails the transaction, and the
// pool rolls it back on the kept connection before the next caller gets it.
RUVIA_TEST(db_rejected_transaction_statement_rolls_back_on_the_kept_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.status_ = 'E', .error_ = "syntax error at or near \"SELEC\""},
            {.column_ = "", .command_ = "ROLLBACK"},
            {.rows_ = {"after"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    bool transaction_failed = false;
    std::string after;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
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
    RUVIA_CHECK(code == db_error::code_type::statement_failed);
    RUVIA_CHECK(transaction_failed);
    RUVIA_CHECK(after == "after");
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{4});
    if (statements.size() == 4) {
        RUVIA_CHECK(statements[2].sql_ == "ROLLBACK");
    }
}

// A COPY result leaves the connection in COPY state with no ReadyForQuery to
// drain to, so the statement fails at once and the pool closes the slot: the
// next statement runs on a fresh connection.
RUVIA_TEST(db_copy_result_fails_the_statement_and_closes_the_connection) {
    using namespace ruvia;
    using direction = test::backend_startup_peer::postgres_copy_direction;
    for (const auto copy : {direction::input, direction::output}) {
        for (const auto kind : {statement_kind::query, statement_kind::execute}) {
            test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
                test::backend_startup_peer::after_startup::reaccept, {{.copy_ = copy}, {.rows_ = {"fresh"}}});
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            db_client client(attachment.loop(), peer.config());
            std::optional<db_error::code_type> code;
            std::string fresh;
            test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
                auto handle = connected.handle();
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
            RUVIA_CHECK(code == db_error::code_type::protocol_error);
            RUVIA_CHECK(fresh == "fresh");
            RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
            RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{2});
        }
    }
}
#endif
