#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace pipeline_test {

using peer_type = ruvia::test::backend_startup_peer;

// What one concurrent caller observed. Failures are recorded rather than
// thrown so every caller in a scope finishes on its own.
struct outcome final {
    std::string value_;
    std::uint64_t affected_rows_{0};
    std::optional<ruvia::db_error::code_type> error_;
    std::string sql_state_;
};

inline void record_failure(const ruvia::db_error& error, outcome& result) {
    result.error_ = error.code();
    result.sql_state_ = error.sql_state().value_or("");
}

inline ruvia::task<void> query(ruvia::db_handle handle, std::string_view sql, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

inline ruvia::task<void> query_id(ruvia::db_handle handle, std::string_view sql, std::int64_t id, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql, id);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

inline ruvia::task<void> execute(ruvia::db_handle handle, std::string_view sql, outcome& result) {
    try {
        result.affected_rows_ = (co_await handle.execute(sql)).affected_rows();
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

inline ruvia::task<void> begin_and_commit(ruvia::db_handle handle, outcome& result) {
    try {
        auto transaction = co_await handle.begin_transaction();
        co_await transaction.commit();
        result.value_ = "committed";
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

[[nodiscard]] inline std::vector<std::string> parsed_sql(peer_type& peer) {
    std::vector<std::string> sql;
    for (const auto& parse : peer.postgres_parses()) {
        sql.push_back(parse.sql_);
    }
    return sql;
}

}  // namespace pipeline_test

// Concurrent statements on a pool of one connection are all sent before the
// first reply arrives -- the peer answers nothing until it has read all four
// -- and each caller gets its own result.
RUVIA_TEST(db_postgresql_concurrent_statements_are_pipelined_on_one_connection) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .batch_ = 4}, {.rows_ = {"b"}}, {.column_ = "", .command_ = "UPDATE 3"},
            {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        // A client that waited for each reply would time out instead.
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        statements.spawn(pipeline_test::execute(handle, "UPDATE c SET v = 1", outcomes[2]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]));
        co_await statements.join();
    });
    for (const auto& result : outcomes) {
        RUVIA_CHECK(!result.error_.has_value());
    }
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK_EQ(outcomes[2].affected_rows_, std::uint64_t{3});
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "UPDATE c SET v = 1", "SELECT v FROM d"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// Each statement ends in its own Sync, so a statement the server rejects
// fails alone; the others and the connection are unaffected.
RUVIA_TEST(db_postgresql_failed_pipelined_statement_does_not_fail_the_others) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .batch_ = 3}, {.error_ = "duplicate key value violates unique constraint", .sql_state_ = "23505"},
            {.rows_ = {"c"}}, {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::execute(handle, "INSERT INTO jobs (key) VALUES ('a')", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "23505");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A pending statement whose caller stops it, or whose timeout passes, leaves
// alone with its usual error -- whether it was reading the connection's
// results (the first) or waiting for another statement to read them (the
// second): the statements around it get their results, its own results are
// discarded, and the connection is kept.
RUVIA_TEST(db_postgresql_abandoned_pipelined_statement_does_not_affect_the_others) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::array<std::string_view, 3> sql{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM c"};
    constexpr std::array<std::string_view, 3> values{"a", "b", "c"};
    for (const std::size_t abandoned_index : {std::size_t{0}, std::size_t{1}}) {
        for (const bool cancel : {true, false}) {
            peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
                {{.rows_ = {"a"}, .deferred_ = true, .batch_ = 3}, {.rows_ = {"b"}}, {.rows_ = {"c"}},
                    {.rows_ = {"d"}}});
            auto& context_value = test::new_test_io_context();
            auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
            db_client client(attachment.loop(), peer.config());
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
            RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
        }
    }
}

// Once every pending statement has been abandoned nobody reads their results,
// so the connection is closed and the next statement connects again.
RUVIA_TEST(db_postgresql_connection_is_closed_when_every_pipelined_statement_is_abandoned) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        {{.rows_ = {"a"}, .deferred_ = true, .batch_ = 2}, {.rows_ = {"b"}}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
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
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// While a transaction waits for the only connection, shared statements stop
// joining its group, so the group drains to the transaction; statements that
// arrive meanwhile follow it.
RUVIA_TEST(db_postgresql_waiting_transaction_is_not_starved_by_pipelined_statements) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .deferred_ = true}, {.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}, {.rows_ = {"b"}}, {.rows_ = {"c"}}, {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
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
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{
                     "SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM b", "SELECT v FROM c", "SELECT v FROM d"}));
}

// Statements and transactions keep their arrival order: statements that
// arrived before a transaction run before it, sharing a connection with one
// another, even when more transactions arrive while they wait. Arrival order:
// a (founds a group), T1, b, c, T2, T3.
RUVIA_TEST(db_postgresql_statements_keep_their_order_against_later_transactions) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    const peer_type::postgres_reply begin{.column_ = "", .command_ = "BEGIN", .status_ = 'T'};
    const peer_type::postgres_reply commit{.column_ = "", .command_ = "COMMIT"};
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .deferred_ = true}, begin, commit, {.rows_ = {"b"}}, {.rows_ = {"c"}}, begin, commit,
            begin, commit});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 6> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope work(connected.worker());
        work.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[3]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[4]));
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[5]));
        peer.release_replies();
        co_await work.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    for (std::size_t index = 3; index < outcomes.size(); ++index) {
        RUVIA_CHECK(outcomes[index].value_ == "committed");
    }
    // The second transaction parses BEGIN and COMMIT as named statements; the
    // third only binds them.
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{
                     "SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM b", "SELECT v FROM c", "BEGIN", "COMMIT"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A lost connection fails every statement still in flight on it, whether or
// not statements ahead of them already got their results.
RUVIA_TEST(db_postgresql_lost_connection_fails_every_pipelined_statement) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    for (const bool first_answered : {false, true}) {
        std::vector<peer_type::postgres_reply> replies;
        if (first_answered) {
            replies.push_back({.rows_ = {"a"}});
        }
        replies.push_back({.batch_ = first_answered ? std::size_t{2} : std::size_t{3}, .drop_ = true});
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
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

// A result the protocol cannot continue from (COPY) fails the statements from
// there on; the one answered before it keeps its result, and the next
// statement connects again.
RUVIA_TEST(db_postgresql_protocol_failure_fails_the_pipelined_statements_after_it) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        {{.rows_ = {"a"}, .batch_ = 3}, {.copy_ = peer_type::postgres_copy_direction::input}, {.rows_ = {"c"}},
            {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "COPY records FROM STDIN", outcomes[1]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::protocol_error);
    RUVIA_CHECK(outcomes[2].error_ == db_error::code_type::protocol_error);
    RUVIA_CHECK(outcomes[3].value_ == "d");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// More statements than one connection keeps in flight wait for room in FIFO
// order: none starves, and each is sent in the order its caller arrived.
RUVIA_TEST(db_postgresql_statements_beyond_pipeline_depth_are_sent_in_arrival_order) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::size_t statement_count = 64;
    std::vector<peer_type::postgres_reply> replies;
    std::vector<std::string> sql;
    for (std::size_t index = 0; index < statement_count; ++index) {
        replies.push_back({.rows_ = {std::to_string(index)}});
        sql.push_back("SELECT v FROM t" + std::to_string(index));
    }
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::vector<pipeline_test::outcome> outcomes(statement_count);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        for (std::size_t index = 0; index < statement_count; ++index) {
            statements.spawn(pipeline_test::query(handle, sql[index], outcomes[index]));
        }
        co_await statements.join();
    });
    for (std::size_t index = 0; index < statement_count; ++index) {
        RUVIA_CHECK(!outcomes[index].error_.has_value());
        RUVIA_CHECK(outcomes[index].value_ == std::to_string(index));
    }
    RUVIA_CHECK(pipeline_test::parsed_sql(peer) == sql);
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A statement waiting for room is bounded like an acquire: it times out after
// the acquire timeout, while the statements already in flight complete.
RUVIA_TEST(db_postgresql_statement_waiting_for_pipeline_room_times_out_like_an_acquire) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::size_t statement_count = 64;
    std::vector<peer_type::postgres_reply> replies;
    std::vector<std::string> sql;
    for (std::size_t index = 0; index < statement_count; ++index) {
        replies.push_back({.rows_ = {std::to_string(index)}, .deferred_ = index == 0});
        sql.push_back("SELECT v FROM t" + std::to_string(index));
    }
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto config = peer.config();
    config.acquire_timeout_ = std::chrono::milliseconds(100);
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config);
    std::vector<pipeline_test::outcome> outcomes(statement_count);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle();
        task_scope statements(connected.worker());
        for (std::size_t index = 0; index < statement_count; ++index) {
            statements.spawn(pipeline_test::query(handle, sql[index], outcomes[index]));
        }
        // Worker timers fire in deadline order, so every waiting statement has
        // timed out before the replies are released.
        (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(200));
        peer.release_replies();
        co_await statements.join();
    });
    std::size_t completed = 0;
    while (completed < statement_count && !outcomes[completed].error_.has_value()) {
        RUVIA_CHECK(outcomes[completed].value_ == std::to_string(completed));
        ++completed;
    }
    RUVIA_CHECK(completed > 0);
    RUVIA_CHECK(completed < statement_count);
    for (std::size_t index = completed; index < statement_count; ++index) {
        RUVIA_CHECK(outcomes[index].error_ == db_error::code_type::timeout);
    }
    RUVIA_CHECK_EQ(peer.postgres_parses().size(), completed);
}

// A cached statement found stale while others are in flight behind it runs
// once more, sent again behind them; the statement after it is unaffected.
RUVIA_TEST(db_postgresql_stale_pipelined_statement_is_retried_behind_the_others) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id = $1";
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}}, {.rows_ = {"b"}},
            {.error_ = "prepared statement does not exist", .sql_state_ = "26000", .routine_ = "FetchPreparedStatement", .batch_ = 2},
            {.rows_ = {"d"}}, {.rows_ = {"e"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        // Unnamed on its first use, parsed as a named statement on its second.
        co_await pipeline_test::query_id(handle, cached_sql, 1, outcomes[0]);
        co_await pipeline_test::query_id(handle, cached_sql, 1, outcomes[1]);
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query_id(handle, cached_sql, 2, outcomes[2]));
        statements.spawn(pipeline_test::query_id(handle, cached_sql, 3, outcomes[3]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "e");
    RUVIA_CHECK(outcomes[3].value_ == "d");
    // The retry runs unnamed: the stale statement is parsed again on its next use.
    RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{3});
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A Parse that fails leaves no server statement, so a statement sent behind it
// that binds the same name finds none (26000): that one runs again, unnamed,
// while the failed Parse reaches its own caller.
RUVIA_TEST(db_postgresql_failed_parse_makes_later_binds_of_its_name_run_again) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id = $1";
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}},
            {.error_ = "relation \"records\" does not exist", .sql_state_ = "42P01", .batch_ = 2, .parse_fails_ = true},
            {}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        co_await pipeline_test::query_id(handle, cached_sql, 1, outcomes[0]);
        // The second use parses the named statement; the third binds it.
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query_id(handle, cached_sql, 2, outcomes[1]));
        statements.spawn(pipeline_test::query_id(handle, cached_sql, 3, outcomes[2]));
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::statement_failed);
    RUVIA_CHECK(outcomes[1].sql_state_ == "42P01");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    // Unnamed, the failed named Parse, and the retry unnamed again.
    RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{3});
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// close() reaches statements in every phase -- reading the results, waiting
// for the reader, and parked beyond the pipeline depth -- and the client
// still shuts down cleanly.
RUVIA_TEST(db_postgresql_close_ends_pipelined_statements_in_every_phase) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::size_t statement_count = 40;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"x"}, .deferred_ = true}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
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
RUVIA_TEST(db_postgresql_read_timeout_fails_every_pipelined_statement) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        {{.rows_ = {"a"}, .deferred_ = true, .batch_ = 3}, {.rows_ = {"b"}}, {.rows_ = {"c"}}, {.rows_ = {"d"}}});
    auto config = peer.config();
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

// A transaction that stops waiting no longer holds back the statements that
// arrived after it: they join the group in flight at once.
RUVIA_TEST(db_postgresql_cancelled_waiting_transaction_lets_later_statements_join) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .deferred_ = true}, {.rows_ = {"b"}}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        stop_source stop;
        task_scope work(connected.worker());
        work.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        work.spawn(pipeline_test::begin_and_commit(handle.with_options({.stop_token_ = stop.token()}), outcomes[3]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        stop.request_stop();
        peer.release_replies();
        co_await work.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM c"}));
}

// A statement waiting behind a transaction that is cancelled leaves its place;
// the statement behind it still follows the transaction.
RUVIA_TEST(db_postgresql_cancelled_statement_waiting_behind_a_transaction_leaves_its_place) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .deferred_ = true}, {.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
            {.column_ = "", .command_ = "COMMIT"}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        stop_source stop;
        task_scope work(connected.worker());
        work.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[3]));
        work.spawn(pipeline_test::query(handle.with_options({.stop_token_ = stop.token()}), "SELECT v FROM b", outcomes[1]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        stop.request_stop();
        peer.release_replies();
        co_await work.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "a");
    RUVIA_CHECK(outcomes[1].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK(outcomes[2].value_ == "c");
    RUVIA_CHECK(outcomes[3].value_ == "committed");
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM c"}));
}

// Two connections, each with a statement held in flight. A transaction gets
// the first connection to free up; once it has, the statement that arrived
// after it joins the group still busy on the other connection rather than
// waiting for a connection of its own.
RUVIA_TEST(db_postgresql_served_transaction_lets_a_later_statement_join_the_other_connection) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    // Connections are scripted in accept order; whichever statement each one
    // carries answers "first".
    peer_type peer({{{.rows_ = {"first"}, .hold_ = 1}, {.column_ = "", .command_ = "BEGIN", .status_ = 'T'},
                        {.column_ = "", .command_ = "COMMIT"}, {.rows_ = {"on the first connection"}}},
        {{.rows_ = {"first"}, .hold_ = 2}, {.rows_ = {"on the second connection"}}}});
    auto config = peer.config();
    config.pool_size_per_worker_ = 2;
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), config);
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        task_scope statements(connected.worker());
        task_scope transactions(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM a", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
        transactions.spawn(pipeline_test::begin_and_commit(handle, outcomes[3]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM c", outcomes[2]));
        peer.release_hold(1);
        co_await transactions.join();
        peer.release_hold(2);
        co_await statements.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "first");
    RUVIA_CHECK(outcomes[1].value_ == "first");
    RUVIA_CHECK(outcomes[2].value_ == "on the second connection");
    RUVIA_CHECK(outcomes[3].value_ == "committed");
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}

// A group whose oldest statement was abandoned is stalled behind it: a new
// statement is not sent behind that one but waits for room elsewhere, here
// until its own timeout.
RUVIA_TEST(db_postgresql_statement_is_not_sent_behind_an_abandoned_one) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"a"}, .deferred_ = true, .batch_ = 2}, {.rows_ = {"b"}}, {.rows_ = {"d"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 4> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle();
        const auto short_lived = handle.with_options({.timeout_ = std::chrono::milliseconds(100)});
        task_scope statements(connected.worker());
        {
            task_scope abandoned(connected.worker());
            abandoned.spawn(pipeline_test::query(short_lived, "SELECT v FROM a", outcomes[0]));
            statements.spawn(pipeline_test::query(handle, "SELECT v FROM b", outcomes[1]));
            co_await abandoned.join();
        }
        co_await pipeline_test::query(short_lived, "SELECT v FROM c", outcomes[2]);
        peer.release_replies();
        co_await statements.join();
        co_await pipeline_test::query(handle, "SELECT v FROM d", outcomes[3]);
    });
    RUVIA_CHECK(outcomes[0].error_ == db_error::code_type::timeout);
    RUVIA_CHECK(outcomes[1].value_ == "b");
    RUVIA_CHECK(outcomes[2].error_ == db_error::code_type::timeout);
    RUVIA_CHECK(outcomes[3].value_ == "d");
    // The statement that timed out waiting was never sent.
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM d"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
}

// A statement that leaves its connection inside a transaction block (BEGIN
// through execute()) must not hand that block to the next caller: the
// connection is closed when its group dissolves.
RUVIA_TEST(db_postgresql_connection_left_inside_a_transaction_block_is_closed) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'}, {.rows_ = {"fresh"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
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

// A transaction stopped while it waits leaves the lease queue at once, though
// its own resumption comes a turn later. A connection released in between
// goes to the statement that waited behind it, not to a transaction that
// arrives after that statement.
RUVIA_TEST(db_postgresql_connection_released_after_a_waiting_transaction_stops_goes_to_the_parked_statement) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    // The held transaction's connection is closed when it is abandoned, so
    // the rest run on a second connection.
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'}, {.rows_ = {"s"}},
            {.column_ = "", .command_ = "BEGIN", .status_ = 'T'}, {.column_ = "", .command_ = "COMMIT"}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        std::optional<db_transaction> held;
        held.emplace(co_await handle.begin_transaction());
        stop_source stop;
        task_scope work(connected.worker());
        work.spawn(pipeline_test::begin_and_commit(handle.with_options({.stop_token_ = stop.token()}), outcomes[2]));
        work.spawn(pipeline_test::query(handle, "SELECT v FROM s", outcomes[0]));
        // All in one turn: the stop, the release and a later transaction.
        stop.request_stop();
        held.reset();
        work.spawn(pipeline_test::begin_and_commit(handle, outcomes[1]));
        co_await work.join();
    });
    RUVIA_CHECK(outcomes[0].value_ == "s");
    RUVIA_CHECK(outcomes[1].value_ == "committed");
    RUVIA_CHECK(outcomes[2].error_ == db_error::code_type::cancelled);
    RUVIA_CHECK((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"BEGIN", "SELECT v FROM s", "BEGIN", "COMMIT"}));
    RUVIA_CHECK_EQ(peer.startups(), std::size_t{2});
}
#endif
