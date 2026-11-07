#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
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
#include "pipeline_support.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace pipeline_test {

[[nodiscard]] inline std::vector<std::string> parsed_sql(peer_type& peer) {
    std::vector<std::string> sql;
    for (const auto& parse : peer.postgres_parses()) {
        sql.push_back(parse.sql_);
    }
    return sql;
}

}  // namespace pipeline_test

TEST(Ruvia, db_postgresql_exhausted_stream_preserves_buffered_results_and_connection) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    for (const bool empty : {false, true}) {
        std::vector<peer_type::postgres_reply> replies;
        // The buffered SQL is first uncached, then prepared, then a cache hit.
        for (int iteration = 0; iteration < 3; ++iteration) {
            replies.push_back({.rows_ = empty ? std::vector<std::string>{} : std::vector<std::string>{"stream-a", "stream-b"},
                .command_ = empty ? "SELECT 0" : "SELECT 2"});
            replies.push_back({.rows_ = {"buffered-a", "buffered-b"}, .command_ = "SELECT 2"});
        }
        peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
        auto& context = test::new_test_io_context();
        auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
        auto config = peer.config();
        config.pool_size_per_worker_ = 1;
        db_client client(attachment.loop(), config);
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            for (int iteration = 0; iteration < 3; ++iteration) {
                auto stream = co_await handle.query_stream("SELECT v FROM streamed WHERE id > $1", iteration);
                std::vector<std::string> values;
                while (auto row = co_await stream.read()) {
                    values.emplace_back((*row)["v"].as<std::string_view>().value_or(""));
                }
                EXPECT_TRUE((values == (empty ? std::vector<std::string>{} : std::vector<std::string>{"stream-a", "stream-b"})));
                EXPECT_TRUE(!stream.active());
                const auto rows = co_await handle.query("SELECT v FROM buffered WHERE id > $1", iteration);
                EXPECT_EQ(rows.size(), std::size_t{2});
                if (rows.size() == 2) {
                    EXPECT_TRUE(rows[0]["v"].as<std::string_view>() == "buffered-a");
                    EXPECT_TRUE(rows[1]["v"].as<std::string_view>() == "buffered-b");
                }
            }
        });
        EXPECT_EQ(peer.startups(), std::size_t{1});
    }
}

TEST(Ruvia, db_postgresql_rejected_stream_transaction_does_not_reach_the_next_owner) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::string_view cached_sql = "SELECT current_setting('transaction_read_only') AS v WHERE $1::integer > 0";
    for (const bool next_is_transaction : {false, true}) {
        std::vector<peer_type::postgres_reply> original{
            {.rows_ = {"off"}}, {.rows_ = {"off"}},
            {.column_ = "", .command_ = "BEGIN", .status_ = 'T'}};
        std::vector<peer_type::postgres_reply> replacement;
        if (next_is_transaction) {
            const peer_type::postgres_reply begin{.column_ = "", .command_ = "BEGIN", .status_ = 'T'};
            original.push_back(begin);
            replacement.push_back(begin);
        }
        // The original session is still read-only after a nested BEGIN; a
        // fresh connection must also prepare the cached statement anew.
        original.push_back({.rows_ = {"on"}, .status_ = 'T'});
        replacement.push_back({.rows_ = {"off"}, .status_ = next_is_transaction ? 'T' : 'I'});
        if (next_is_transaction) {
            const peer_type::postgres_reply commit{.column_ = "", .command_ = "COMMIT"};
            original.push_back(commit);
            replacement.push_back(commit);
        }
        peer_type peer(std::vector<std::vector<peer_type::postgres_reply>>{
            std::move(original), std::move(replacement)});
        auto& context = test::new_test_io_context();
        auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
        auto config = peer.config();
        config.pool_size_per_worker_ = 1;
        db_client client(attachment.loop(), config);
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
            // Second use creates a named statement owned by this session.
            for (int iteration = 0; iteration < 2; ++iteration) {
                const auto rows = co_await handle.query(cached_sql, 1);
                EXPECT_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    EXPECT_TRUE(rows[0]["v"].as<std::string_view>() == "off");
                }
            }
            auto stream = co_await handle.query_stream("BEGIN READ ONLY");
            bool rejected = false;
            try {
                (void)co_await stream.read();
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
            EXPECT_TRUE(!stream.active());
            if (next_is_transaction) {
                auto transaction = co_await handle.begin_transaction();
                const auto rows = co_await transaction.query(cached_sql, 1);
                EXPECT_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    EXPECT_TRUE(rows[0]["v"].as<std::string_view>() == "off");
                }
                co_await transaction.commit();
                EXPECT_TRUE(!transaction.active());
            } else {
                const auto rows = co_await handle.query(cached_sql, 1);
                EXPECT_EQ(rows.size(), std::size_t{1});
                if (!rows.empty()) {
                    EXPECT_TRUE(rows[0]["v"].as<std::string_view>() == "off");
                }
            }
        });
        EXPECT_EQ(peer.startups(), std::size_t{2});
    }
}

TEST(Ruvia, db_postgresql_rejected_idle_stream_preserves_cached_results_and_connection) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id > $1";
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"before"}}, {.rows_ = {"prepared"}},
            {.column_ = "", .command_ = "UPDATE 1"},
            {.rows_ = {"after-a", "after-b"}, .command_ = "SELECT 2"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.pool_size_per_worker_ = 1;
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        for (int iteration = 0; iteration < 2; ++iteration) {
            const auto rows = co_await handle.query(cached_sql, iteration);
            EXPECT_EQ(rows.size(), std::size_t{1});
        }
        auto stream = co_await handle.query_stream("UPDATE records SET v = 'changed'");
        bool rejected = false;
        try {
            (void)co_await stream.read();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_TRUE(!stream.active());
        const auto rows = co_await handle.query(cached_sql, 0);
        EXPECT_EQ(rows.size(), std::size_t{2});
        if (rows.size() == 2) {
            EXPECT_TRUE(rows[0]["v"].as<std::string_view>() == "after-a");
            EXPECT_TRUE(rows[1]["v"].as<std::string_view>() == "after-b");
        }
    });
    EXPECT_EQ(peer.startups(), std::size_t{1});
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{std::string(cached_sql), std::string(cached_sql), "UPDATE records SET v = 'changed'"}));
}

TEST(Ruvia, db_postgresql_exhausted_stream_allows_concurrent_pipeline_reuse) {
    using namespace ruvia;
    using pipeline_test::peer_type;
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        {{.rows_ = {"stream"}},
            {.rows_ = {"first"}, .batch_ = 3},
            {.rows_ = {"second"}},
            {.column_ = "", .command_ = "UPDATE 2"}});
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.pool_size_per_worker_ = 1;
    db_client client(attachment.loop(), config);
    std::array<pipeline_test::outcome, 3> outcomes;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        const auto handle = connected.handle({.timeout_ = std::chrono::seconds(5)});
        auto stream = co_await handle.query_stream("SELECT v FROM streamed");
        const auto row = co_await stream.read();
        EXPECT_TRUE(row && (*row)["v"].as<std::string_view>() == "stream");
        EXPECT_TRUE(!(co_await stream.read()));
        task_scope statements(connected.worker());
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM first", outcomes[0]));
        statements.spawn(pipeline_test::query(handle, "SELECT v FROM second", outcomes[1]));
        statements.spawn(pipeline_test::execute(handle, "UPDATE records SET v = 'done'", outcomes[2]));
        co_await statements.join();
    });
    for (const auto& result : outcomes) {
        EXPECT_TRUE(!result.error_.has_value());
    }
    EXPECT_TRUE(outcomes[0].value_ == "first");
    EXPECT_TRUE(outcomes[1].value_ == "second");
    EXPECT_EQ(outcomes[2].affected_rows_, std::uint64_t{2});
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// Concurrent statements on a pool of one connection are all sent before the
// first reply arrives -- the peer answers nothing until it has read all four
// -- and each caller gets its own result.
TEST(Ruvia, db_postgresql_concurrent_statements_are_pipelined_on_one_connection) {
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
        EXPECT_TRUE(!result.error_.has_value());
    }
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_EQ(outcomes[2].affected_rows_, std::uint64_t{3});
    EXPECT_TRUE(outcomes[3].value_ == "d");
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "UPDATE c SET v = 1", "SELECT v FROM d"}));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// Each statement ends in its own Sync, so a statement the server rejects
// fails alone; the others and the connection are unaffected.
TEST(Ruvia, db_postgresql_failed_pipelined_statement_does_not_fail_the_others) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[1].sql_state_ == "23505");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE(outcomes[3].value_ == "d");
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A pending statement whose caller stops it, or whose timeout passes, leaves
// alone with its usual error -- whether it was reading the connection's
// results (the first) or waiting for another statement to read them (the
// second): the statements around it get their results, its own results are
// discarded, and the connection is kept.
TEST(Ruvia, db_postgresql_abandoned_pipelined_statement_does_not_affect_the_others) {
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
                    EXPECT_TRUE(outcomes[index].error_ ==
                                (cancel ? db_error::code_type::cancelled : db_error::code_type::timeout));
                    EXPECT_TRUE(outcomes[index].value_.empty());
                } else {
                    EXPECT_TRUE(outcomes[index].value_ == values[index]);
                }
            }
            EXPECT_TRUE(outcomes[3].value_ == "d");
            EXPECT_EQ(peer.startups(), std::size_t{1});
        }
    }
}

// Once every pending statement has been abandoned nobody reads their results,
// so the connection is closed and the next statement connects again.
TEST(Ruvia, db_postgresql_connection_is_closed_when_every_pipelined_statement_is_abandoned) {
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
    EXPECT_TRUE(outcomes[0].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

// While a transaction waits for the only connection, shared statements stop
// joining its group, so the group drains to the transaction; statements that
// arrive meanwhile follow it.
TEST(Ruvia, db_postgresql_waiting_transaction_is_not_starved_by_pipelined_statements) {
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
    EXPECT_TRUE(outcomes[4].value_ == "committed");
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE(outcomes[3].value_ == "d");
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{
                     "SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM b", "SELECT v FROM c", "SELECT v FROM d"}));
}

// Statements and transactions keep their arrival order: statements that
// arrived before a transaction run before it, sharing a connection with one
// another, even when more transactions arrive while they wait. Arrival order:
// a (founds a group), T1, b, c, T2, T3.
TEST(Ruvia, db_postgresql_statements_keep_their_order_against_later_transactions) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    for (std::size_t index = 3; index < outcomes.size(); ++index) {
        EXPECT_TRUE(outcomes[index].value_ == "committed");
    }
    // The second transaction parses BEGIN and COMMIT as named statements; the
    // third only binds them.
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{
                     "SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM b", "SELECT v FROM c", "BEGIN", "COMMIT"}));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A lost connection fails every statement still in flight on it, whether or
// not statements ahead of them already got their results.
TEST(Ruvia, db_postgresql_lost_connection_fails_every_pipelined_statement) {
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
            EXPECT_TRUE(outcomes[0].value_ == "a");
        } else {
            EXPECT_TRUE(outcomes[0].error_ == db_error::code_type::io_error);
        }
        EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::io_error);
        EXPECT_TRUE(outcomes[2].error_ == db_error::code_type::io_error);
    }
}

// A result the protocol cannot continue from (COPY) fails the statements from
// there on; the one answered before it keeps its result, and the next
// statement connects again.
TEST(Ruvia, db_postgresql_protocol_failure_fails_the_pipelined_statements_after_it) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::protocol_error);
    EXPECT_TRUE(outcomes[2].error_ == db_error::code_type::protocol_error);
    EXPECT_TRUE(outcomes[3].value_ == "d");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

// More statements than one connection keeps in flight wait for room in FIFO
// order: none starves, and each is sent in the order its caller arrived.
TEST(Ruvia, db_postgresql_statements_beyond_pipeline_depth_are_sent_in_arrival_order) {
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
        EXPECT_TRUE(!outcomes[index].error_.has_value());
        EXPECT_TRUE(outcomes[index].value_ == std::to_string(index));
    }
    EXPECT_TRUE(pipeline_test::parsed_sql(peer) == sql);
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A statement waiting for room is bounded like an acquire: it times out after
// the acquire timeout, while the statements already in flight complete.
TEST(Ruvia, db_postgresql_statement_waiting_for_pipeline_room_times_out_like_an_acquire) {
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
        EXPECT_TRUE(outcomes[completed].value_ == std::to_string(completed));
        ++completed;
    }
    EXPECT_TRUE(completed > 0);
    EXPECT_TRUE(completed < statement_count);
    for (std::size_t index = completed; index < statement_count; ++index) {
        EXPECT_TRUE(outcomes[index].error_ == db_error::code_type::timeout);
    }
    EXPECT_EQ(peer.postgres_parses().size(), completed);
}

// A cached statement found stale while others are in flight behind it runs
// once more, sent again behind them; the statement after it is unaffected.
TEST(Ruvia, db_postgresql_stale_pipelined_statement_is_retried_behind_the_others) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "e");
    EXPECT_TRUE(outcomes[3].value_ == "d");
    // The retry runs unnamed: the stale statement is parsed again on its next use.
    EXPECT_EQ(peer.postgres_parses().size(), std::size_t{3});
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A Parse that fails leaves no server statement, so a statement sent behind it
// that binds the same name finds none (26000): that one runs again, unnamed,
// while the failed Parse reaches its own caller.
TEST(Ruvia, db_postgresql_failed_parse_makes_later_binds_of_its_name_run_again) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::statement_failed);
    EXPECT_TRUE(outcomes[1].sql_state_ == "42P01");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    // Unnamed, the failed named Parse, and the retry unnamed again.
    EXPECT_EQ(peer.postgres_parses().size(), std::size_t{3});
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// close() reaches statements in every phase -- reading the results, waiting
// for the reader, and parked beyond the pipeline depth -- and the client
// still shuts down cleanly.
TEST(Ruvia, db_postgresql_close_ends_pipelined_statements_in_every_phase) {
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
        EXPECT_TRUE(result.error_ == db_error::code_type::cancelled || result.error_ == db_error::code_type::closing);
    }
}

// A read timeout means the connection made no progress: every statement in
// flight on it fails, and the next statement connects again.
TEST(Ruvia, db_postgresql_read_timeout_fails_every_pipelined_statement) {
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
        EXPECT_TRUE(outcomes[index].error_ == db_error::code_type::timeout);
    }
    EXPECT_TRUE(outcomes[3].value_ == "d");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

// A transaction that stops waiting no longer holds back the statements that
// arrived after it: they join the group in flight at once.
TEST(Ruvia, db_postgresql_cancelled_waiting_transaction_lets_later_statements_join) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE(outcomes[3].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM c"}));
}

// A statement waiting behind a transaction that is cancelled leaves its place;
// the statement behind it still follows the transaction.
TEST(Ruvia, db_postgresql_cancelled_statement_waiting_behind_a_transaction_leaves_its_place) {
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
    EXPECT_TRUE(outcomes[0].value_ == "a");
    EXPECT_TRUE(outcomes[1].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE(outcomes[2].value_ == "c");
    EXPECT_TRUE(outcomes[3].value_ == "committed");
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "BEGIN", "COMMIT", "SELECT v FROM c"}));
}

// Two connections, each with a statement held in flight. A transaction gets
// the first connection to free up; once it has, the statement that arrived
// after it joins the group still busy on the other connection rather than
// waiting for a connection of its own.
TEST(Ruvia, db_postgresql_served_transaction_lets_a_later_statement_join_the_other_connection) {
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
    EXPECT_TRUE(outcomes[0].value_ == "first");
    EXPECT_TRUE(outcomes[1].value_ == "first");
    EXPECT_TRUE(outcomes[2].value_ == "on the second connection");
    EXPECT_TRUE(outcomes[3].value_ == "committed");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

// A group whose oldest statement was abandoned is stalled behind it: a new
// statement is not sent behind that one but waits for room elsewhere, here
// until its own timeout.
TEST(Ruvia, db_postgresql_statement_is_not_sent_behind_an_abandoned_one) {
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
    EXPECT_TRUE(outcomes[0].error_ == db_error::code_type::timeout);
    EXPECT_TRUE(outcomes[1].value_ == "b");
    EXPECT_TRUE(outcomes[2].error_ == db_error::code_type::timeout);
    EXPECT_TRUE(outcomes[3].value_ == "d");
    // The statement that timed out waiting was never sent.
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"SELECT v FROM a", "SELECT v FROM b", "SELECT v FROM d"}));
    EXPECT_EQ(peer.startups(), std::size_t{1});
}

// A statement that leaves its connection inside a transaction block (BEGIN
// through execute()) must not hand that block to the next caller: the
// connection is closed when its group dissolves.
TEST(Ruvia, db_postgresql_connection_left_inside_a_transaction_block_is_closed) {
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
    EXPECT_TRUE(!outcomes[0].error_.has_value());
    EXPECT_TRUE(outcomes[1].value_ == "fresh");
    EXPECT_EQ(peer.startups(), std::size_t{2});
}

// A transaction stopped while it waits leaves the lease queue at once, though
// its own resumption comes a turn later. A connection released in between
// goes to the statement that waited behind it, not to a transaction that
// arrives after that statement.
TEST(Ruvia, db_postgresql_connection_released_after_a_waiting_transaction_stops_goes_to_the_parked_statement) {
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
    EXPECT_TRUE(outcomes[0].value_ == "s");
    EXPECT_TRUE(outcomes[1].value_ == "committed");
    EXPECT_TRUE(outcomes[2].error_ == db_error::code_type::cancelled);
    EXPECT_TRUE((pipeline_test::parsed_sql(peer) ==
                 std::vector<std::string>{"BEGIN", "SELECT v FROM s", "BEGIN", "COMMIT"}));
    EXPECT_EQ(peer.startups(), std::size_t{2});
}
#endif
