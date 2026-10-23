#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "pipeline_support.h"
#include "test_harness.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_MARIADB
namespace mariadb_pipeline_test {

using pipeline_test::peer_type;
using reply = peer_type::maria_reply;

[[nodiscard]] inline ruvia::db_config config(peer_type& peer) {
    auto value = peer.config();
    value.driver_ = ruvia::db_driver::mariadb;
    value.username_ = "test";
    return value;
}

inline ruvia::task<void> query_with(
    ruvia::db_handle handle, std::string_view sql, std::string_view param, pipeline_test::outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql, param);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        pipeline_test::record_failure(error, result);
    }
}

}  // namespace mariadb_pipeline_test

// Concurrent statements on a pool of one connection are all sent before the
// first reply arrives -- the peer answers nothing until it has read all four
// commands -- and each caller gets its own result.
RUVIA_TEST(db_mariadb_concurrent_statements_are_pipelined_on_one_connection) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .batch_ = 4}, {.rows_ = {"b"}}, {.column_ = "", .affected_rows_ = 3},
                       {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        statements.spawn(pipeline_test::execute(handle, "UPDATE c SET v = 1", outcomes[2]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK_EQ(outcomes[2].affected_rows_, std::uint64_t{3});
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK((peer.maria_queries() ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "UPDATE c SET v = 1", "SELECT v FROM d"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// The server runs each command on its own, so a statement it rejects fails
// alone with its own error; the statements around it and the connection are
// unaffected.
RUVIA_TEST(db_mariadb_failed_pipelined_statement_does_not_fail_the_others) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .batch_ = 3},
                       {.error_ = "Table 'bench.b' doesn't exist", .code_ = 1146, .sql_state_ = "42S02"},
                       {.rows_ = {"c"}}, {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "42S02");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// Affected rows and insert ids are read from each statement's own response,
// in send order.
RUVIA_TEST(db_mariadb_pipelined_executes_report_their_own_affected_rows_and_insert_ids) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.column_ = "", .affected_rows_ = 1, .insert_id_ = 7, .batch_ = 3},
                       {.column_ = "", .affected_rows_ = 2}, {.column_ = "", .affected_rows_ = 300, .insert_id_ = 9}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::execute(handle, "INSERT INTO a (v) VALUES (1)", outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, "UPDATE b SET v = 2", outcomes[1]));
        statements.spawn(pipeline_test::execute(handle, "INSERT INTO c (v) SELECT v FROM d", outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK_EQ(outcomes[0].affected_rows_, std::uint64_t{1});
    RUVIA_CHECK(outcomes[0].insert_id_ == std::uint64_t{7});
    RUVIA_CHECK_EQ(outcomes[1].affected_rows_, std::uint64_t{2});
    RUVIA_CHECK(!outcomes[1].insert_id_.has_value());
    RUVIA_CHECK_EQ(outcomes[2].affected_rows_, std::uint64_t{300});
    RUVIA_CHECK(outcomes[2].insert_id_ == std::uint64_t{9});
}

// A procedure call ends with a status result after its row set; it is read
// before the next statement's response, which follows unaffected.
RUVIA_TEST(db_mariadb_procedure_results_are_drained_before_the_next_statement) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"p"}, .procedure_ = true, .batch_ = 2}, {.rows_ = {"after"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 2> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "CALL p()", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "p");
    RUVIA_CHECK(outcomes[1].value_ == "after");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A row set execute() did not ask for, or a missing one query() needs,
// refuses that statement once its response was read; the next statement on
// the connection is unaffected.
RUVIA_TEST(db_mariadb_pipelined_statement_of_the_wrong_shape_is_refused_alone) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"x"}, .batch_ = 3}, {.column_ = "", .affected_rows_ = 1}, {.rows_ = {"z"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::execute(handle, "SELECT v FROM x", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "UPDATE y SET v = 1", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM z", outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].refused_);
    RUVIA_CHECK(outcomes[1].refused_);
    RUVIA_CHECK(outcomes[2].value_ == "z");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A pending statement whose caller stops it, or whose timeout passes, leaves
// alone with its usual error -- whether it was reading the connection's
// responses (the first) or waiting for another statement to read them (the
// second). One stopped before its command went out is never sent; one already
// sent has its response read and discarded. The connection is kept.
RUVIA_TEST(db_mariadb_abandoned_pipelined_statement_does_not_affect_the_others) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    constexpr std::array<std::string_view, 3> sql{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM c"};
    constexpr std::array<std::string_view, 3> values{"a", "b", "c"};
    for (const std::size_t abandoned_index : {std::size_t{0}, std::size_t{1}}) {
        for (const bool cancel : {true, false}) {
            // The first statement's command goes out as it arrives; the others
            // wait for the reader's next turn, which comes after the stop.
            const bool unsent = cancel && abandoned_index == 1;
            std::vector<mariadb_pipeline_test::reply> replies{
                {.rows_ = {"a"}, .deferred_ = true, .batch_ = unsent ? std::size_t{2} : std::size_t{3}}};
            if (!unsent) {
                replies.push_back({.rows_ = {"b"}});
            }
            replies.push_back({.rows_ = {"c"}});
            replies.push_back({.rows_ = {"d"}});
            peer_type peer(std::move(replies), peer_type::after_startup::idle);
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
            std::array<pipeline_test::outcome, 4> outcomes;
            test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
                const auto handle = connected.handle();
                stop_source stop;
                const auto abandoning = cancel ? handle.with_options({.stop_token_ = stop.token()})
                                               : handle.with_options({.timeout_ = std::chrono::milliseconds(100)});
                task_scope others(connected.worker());
                task_scope abandoned(connected.worker());
                for (std::size_t index = 0; index < sql.size(); ++index) {
                    if (index == abandoned_index) {
                        abandoned.spawn(pipeline_test::query(abandoning, sql[index], outcomes[index]));
                    } else {
                        others.spawn(pipeline_test::query(handle, sql[index], outcomes[index]));
                    }
                }
                stop.request_stop();
                // The peer answers nothing before the statement has left.
                co_await abandoned.join();
                peer.release_replies();
                co_await others.join();
                co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
            });
            for (std::size_t index = 0; index < sql.size(); ++index) {
                if (index == abandoned_index) {
                    RUVIA_CHECK(outcomes[index].error_ ==
                                (cancel ? db_error::code_type::cancelled : db_error::code_type::timeout));
                    RUVIA_CHECK(outcomes[index].value_.empty());
                } else {
                    RUVIA_CHECK(outcomes[index].value_ == values[index]);
                }
            }
            RUVIA_CHECK(outcomes[3].value_ == "d");
            const auto queries = peer.maria_queries();
            RUVIA_CHECK_EQ(queries.size(), unsent ? std::size_t{3} : std::size_t{4});
            RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
        }
    }
}

// Once every pending statement has been abandoned nobody reads their
// responses, so the connection is closed and the next statement connects
// again.
RUVIA_TEST(db_mariadb_connection_is_closed_when_every_pipelined_statement_is_abandoned) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .deferred_ = true}, {.rows_ = {"c"}}}, peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle();
        stop_source stop;
        const auto stoppable = handle.with_options({.stop_token_ = stop.token()});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(stoppable, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(stoppable, "SELECT v FROM b", outcomes[1]));
        stop.request_stop();
        co_await statements.join();
        peer.release_replies();
        co_await pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]);
    });
    RUVIA_CHECK(outcomes[0].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK(outcomes[2].value_ == "c");
    // The second statement was stopped before its command went out.
    RUVIA_CHECK((peer.maria_queries() == std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM c"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// While a transaction waits for the only connection, shared statements stop
// joining its group, so the group drains to the transaction; statements that
// arrive meanwhile follow it.
RUVIA_TEST(db_mariadb_waiting_transaction_is_not_starved_by_pipelined_statements) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .deferred_ = true}, {.column_ = "", .status_ = 3}, {.column_ = ""},
                       {.rows_ = {"b"}}, {.rows_ = {"c"}}, {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 5> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope work(connected.worker());
        work.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[4]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]));
        peer.release_replies();
        co_await work.join();
    });
    RUVIA_CHECK(outcomes[4].value_ == "committed");
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK((peer.maria_queries() == std::vector<std::string>{"SELECT v FROM a", "START TRANSACTION", "COMMIT",
                                             "SELECT v FROM b", "SELECT v FROM c", "SELECT v FROM d"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A lost connection fails every statement still in flight on it, whether or
// not statements ahead of them already got their results.
RUVIA_TEST(db_mariadb_lost_connection_fails_every_pipelined_statement) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    for (const bool first_answered : {false, true}) {
        std::vector<mariadb_pipeline_test::reply> replies;
        if (first_answered) {
            replies.push_back({.rows_ = {"a"}});
        }
        replies.push_back({.batch_ = first_answered ? std::size_t{2} : std::size_t{3}, .drop_ = true});
        peer_type peer(std::move(replies), peer_type::after_startup::idle);
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
        std::array<pipeline_test::outcome, 3> outcomes;
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            task_scope statements(connected.worker());
            statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
            statements.spawn(pipeline_test::execute(handle, "UPDATE b SET v = 1", outcomes[1]));
            statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
            co_await statements.join();
        });
        if (first_answered) {
            RUVIA_CHECK(outcomes[0].value_ == "a");
        } else {
            RUVIA_CHECK(outcomes[0].error_ == db_error::code_type::io_error);
        }
        RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::io_error);
        RUVIA_CHECK(outcomes[2].error_ == db_error::code_type::io_error);
    }
}

// close() reaches statements in every phase -- reading the responses, waiting
// for the reader, and parked beyond the pipeline depth -- and the client
// still shuts down cleanly.
RUVIA_TEST(db_mariadb_close_ends_pipelined_statements_in_every_phase) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    constexpr std::size_t statement_count = 40;
    peer_type peer({{.rows_ = {"x"}, .deferred_ = true}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::vector<pipeline_test::outcome> outcomes(statement_count);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle();
        task_scope statements(connected.worker());
        for (auto& result : outcomes) {
            statements.spawn(pipeline_test::query(handle, "SELECT v FROM t", result));
        }
        connected.close();
        co_await statements.join();
    });
    for (const auto& result : outcomes) {
        RUVIA_CHECK(result.error_ == db_error::code_type::cancelled || result.error_ == db_error::code_type::closing);
    }
}

// A read timeout means the connection made no progress: every statement in
// flight on it fails, and the next statement connects again.
RUVIA_TEST(db_mariadb_read_timeout_fails_every_pipelined_statement) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .deferred_ = true, .batch_ = 3}, {.rows_ = {"b"}}, {.rows_ = {"c"}},
                       {.rows_ = {"d"}}},
        peer_type::after_startup::reaccept);
    auto config = mariadb_pipeline_test::config(peer);
    config.read_timeout_ = std::chrono::milliseconds(100);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config);
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle();
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        // The peer answers only once every statement has failed.
        co_await statements.join();
        peer.release_replies();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    for (std::size_t index = 0; index < 3; ++index) {
        RUVIA_CHECK(outcomes[index].error_ == db_error::code_type::timeout);
    }
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// A statement that leaves its connection inside a transaction (BEGIN through
// execute()) must not hand it to the next caller: the connection is closed
// when its group dissolves.
RUVIA_TEST(db_mariadb_connection_left_inside_a_transaction_is_closed) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.column_ = "", .status_ = 3}, {.rows_ = {"fresh"}}}, peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 2> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::execute(handle, "BEGIN", outcomes[0]);
        co_await pipeline_test::query(handle, "SELECT v FROM t", outcomes[1]);
    });
    RUVIA_CHECK(!outcomes[0].error_.has_value());
    RUVIA_CHECK(outcomes[1].value_ == "fresh");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// Parameters are interpolated with the escaping of the connection the
// statement is sent on.
RUVIA_TEST(db_mariadb_pipelined_parameters_are_interpolated) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"x"}}}, peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    pipeline_test::outcome result;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query_id(handle, "SELECT v FROM t WHERE id = ?", 42, result);
    });
    RUVIA_CHECK(result.value_ == "x");
    RUVIA_CHECK((peer.maria_queries() == std::vector<std::string>{"SELECT v FROM t WHERE id = 42"}));
}

// MariaDB numbers server errors above the client library's range as well
// (4025, a violated CHECK constraint): the statement fails alone with its
// own error, and the connection is kept.
RUVIA_TEST(db_mariadb_pipelined_server_error_above_the_client_range_fails_alone) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .batch_ = 3},
                       {.error_ = "CONSTRAINT `t.v` failed for `db`.`t`", .code_ = 4025, .sql_state_ = "23000"},
                       {.rows_ = {"c"}}, {.rows_ = {"d"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, "INSERT INTO t (v) VALUES (-1)", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "23000");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A server error after which the server ends the session (1927, the
// connection was killed) leaves nothing to read the next response from: it
// fails as an I/O error, and the connection is not used again.
RUVIA_TEST(db_mariadb_server_error_ending_the_session_closes_the_connection) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.rows_ = {"a"}, .batch_ = 2},
                       {.error_ = "Connection was killed", .code_ = 1927, .sql_state_ = "70100"},
                       {.rows_ = {"fresh"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::io_error);
    RUVIA_CHECK(outcomes[2].value_ == "fresh");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// A statement pipelined behind a change of sql_mode is read in the new mode.
// A string without a backslash is quoted alike in every mode and goes out
// with the SET -- the peer answers nothing before it has read both -- while
// one with a backslash waits for the SET's response and is escaped for
// NO_BACKSLASH_ESCAPES, which that response reports.
RUVIA_TEST(db_mariadb_statement_behind_an_sql_mode_change_is_escaped_for_the_new_mode) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    constexpr std::uint16_t no_backslash_escapes = 512;
    peer_type peer({{.column_ = "", .status_ = 2 | no_backslash_escapes, .batch_ = 2},
                       {.rows_ = {"x"}, .status_ = 2 | no_backslash_escapes},
                       {.rows_ = {"y"}, .status_ = 2 | no_backslash_escapes}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::execute(handle, "SET sql_mode = 'NO_BACKSLASH_ESCAPES'", outcomes[0]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", "x' , 1 AS w -- ", outcomes[1]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", "y\\' , 2 AS w -- ", outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK(!outcomes[0].error_.has_value());
    RUVIA_CHECK(outcomes[1].value_ == "x");
    RUVIA_CHECK(outcomes[2].value_ == "y");
    RUVIA_CHECK((peer.maria_queries() ==
                 std::vector<std::string>{"SET sql_mode = 'NO_BACKSLASH_ESCAPES'", "SELECT 'x'' , 1 AS w -- ' AS v",
                     "SELECT 'y\\'' , 2 AS w -- ' AS v"}));
}

// LOAD DATA LOCAL INFILE answers its command with a request for the file and
// reads the file as packets of that command, so it goes out once the
// responses before it have been read, and nothing follows it until its own
// response is in. The peer reads a command sent behind it as file content,
// as the server does.
RUVIA_TEST(db_mariadb_load_data_local_infile_has_the_connection_to_itself) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    const auto path = (std::filesystem::temp_directory_path() / "ruvia_mariadb_load_data_local.txt").generic_string();
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "row1\nrow2\n";
    }
    const auto load = "LOAD DATA LOCAL INFILE '" + path + "' INTO TABLE t";
    peer_type peer({{.rows_ = {"a"}}, {.column_ = "", .affected_rows_ = 2, .local_infile_ = path}, {.rows_ = {"c"}},
                       {.rows_ = {"d"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, load, outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    std::filesystem::remove(path);
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(!outcomes[1].error_.has_value());
    RUVIA_CHECK_EQ(outcomes[1].affected_rows_, std::uint64_t{2});
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK((peer.maria_commands() == std::vector<std::string>{"query:SELECT v FROM a", "query:" + load,
                                              "infile:row1\nrow2\n", "query:SELECT v FROM c", "query:SELECT v FROM d"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// libmariadb sends a file for LOAD DATA LOCAL INFILE only when the command
// starts with LOAD; behind whitespace it answers the server's request with an
// empty file and fails the statement (CR_UNKNOWN_ERROR). Its response was
// read, so only that statement fails: the one behind it, still unsent, runs
// on the same connection.
RUVIA_TEST(db_mariadb_refused_load_data_local_infile_fails_alone) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    const auto load = std::string("  LOAD DATA LOCAL INFILE '/nonexistent/ruvia' INTO TABLE t");
    peer_type peer({{.rows_ = {"a"}}, {.column_ = "", .local_infile_ = "/nonexistent/ruvia"}, {.rows_ = {"c"}}},
        peer_type::after_startup::reaccept);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, load, outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK((peer.maria_commands() == std::vector<std::string>{"query:SELECT v FROM a", "query:" + load,
                                              "infile:", "query:SELECT v FROM c"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A backslash escape that follows a byte above 0x7F depends on the session's
// client character set: in GBK, 0xBF 0x5C is one character, and a backslash
// put before its 0x5C would leave the one after it to escape the closing
// quote. Such a statement reads the character set from the server first,
// even with session tracking off, and is escaped by it; a backslash after an
// ASCII byte needs no character set.
RUVIA_TEST(db_mariadb_backslash_escape_reads_the_sessions_character_set) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    peer_type peer({{.column_ = ""}, {.rows_ = {"gbk"}}, {.rows_ = {"x"}}, {.rows_ = {"y"}}},
        peer_type::after_startup::idle, 1, false);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::execute(handle, "SET NAMES gbk", outcomes[0]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", "\xbf\\' , 1 -- ", outcomes[1]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", "a\\b", outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK(!outcomes[0].error_.has_value());
    RUVIA_CHECK(outcomes[1].value_ == "x");
    RUVIA_CHECK(outcomes[2].value_ == "y");
    RUVIA_CHECK((peer.maria_queries() == std::vector<std::string>{"SET NAMES gbk", "SELECT @@character_set_client",
                                             "SELECT '\xbf\\'' , 1 -- ' AS v", "SELECT 'a\\\\b' AS v"}));
}

// In UTF-8 no byte of a multibyte character is 0x5C, so a backslash after
// one escapes as after an ASCII byte. While session tracking says the client
// character set is still utf8mb4, such a parameter is escaped without asking
// the server; once a report names another one, or a CALL may have turned
// tracking off unreported, the character set is read first again.
RUVIA_TEST(db_mariadb_tracked_utf8mb4_session_escapes_without_reading_its_character_set) {
    using namespace ruvia;
    using mariadb_pipeline_test::peer_type;
    const std::vector<std::pair<std::string, std::string>> gbk{
        {"character_set_client", "gbk"}, {"character_set_connection", "gbk"}};
    const std::vector<std::pair<std::string, std::string>> utf8mb4{
        {"character_set_client", "utf8mb4"}, {"character_set_connection", "utf8mb4"}};
    peer_type peer({{.rows_ = {"x"}}, {.column_ = "", .tracked_ = gbk}, {.rows_ = {"gbk"}}, {.rows_ = {"y"}},
                       {.column_ = "", .tracked_ = utf8mb4}, {.column_ = ""}, {.rows_ = {"utf8mb4"}},
                       {.rows_ = {"z"}}},
        peer_type::after_startup::idle);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), mariadb_pipeline_test::config(peer));
    std::array<pipeline_test::outcome, 6> outcomes;
    const std::string jose = "Jos\xc3\xa9\\x";
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", jose, outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, "SET NAMES gbk", outcomes[1]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS v", "\xbf\\' , 1 -- ", outcomes[2]));
        statements.spawn(pipeline_test::execute(handle, "SET NAMES utf8mb4", outcomes[3]));
        statements.spawn(pipeline_test::execute(handle, "CALL p()", outcomes[4]));
        statements.spawn(mariadb_pipeline_test::query_with(handle, "SELECT ? AS w", jose, outcomes[5]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "x");
    RUVIA_CHECK(outcomes[2].value_ == "y");
    RUVIA_CHECK(outcomes[5].value_ == "z");
    RUVIA_CHECK((peer.maria_queries() ==
                 std::vector<std::string>{"SELECT 'Jos\xc3\xa9\\\\x' AS v", "SET NAMES gbk", "SELECT @@character_set_client",
                     "SELECT '\xbf\\'' , 1 -- ' AS v", "SET NAMES utf8mb4", "CALL p()", "SELECT @@character_set_client",
                     "SELECT 'Jos\xc3\xa9\\\\x' AS w"}));
}
#endif
