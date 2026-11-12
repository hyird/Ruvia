#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {

constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id = $1";

template <typename param_type>
ruvia::task<std::pmr::string> first_value(ruvia::db_handle& handle, param_type param) {
    const ruvia::db_rows rows = co_await handle.query(cached_sql, param);
    co_return std::pmr::string(rows.front()["v"].as<std::string_view>().value_or(""), ruvia::process_resource());
}

using parameter_values = std::pmr::vector<std::optional<std::pmr::string>>;

}  // namespace

// A statement runs unnamed on its first use, is parsed as a named statement on
// its second, and is bound by name afterwards. The server fixes parameter
// types at Parse, so the same SQL with a differently typed parameter is a
// separate statement.
TEST(Ruvia, db_postgresql_repeated_statement_is_parsed_once_per_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("d", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("e", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("f", ruvia::process_resource())}, ruvia::process_resource())}},
            ruvia::process_resource()));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::pmr::vector<std::pmr::string> values{ruvia::process_resource()};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        for (const std::int64_t id : {1, 2, 3}) {
            values.push_back(co_await first_value(handle, id));
        }
        for (const double id : {1.5, 2.5, 3.5}) {
            values.push_back(co_await first_value(handle, id));
        }
    });
    EXPECT_TRUE((values == std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource()), std::pmr::string("b", ruvia::process_resource()), std::pmr::string("c", ruvia::process_resource()), std::pmr::string("d", ruvia::process_resource()), std::pmr::string("e", ruvia::process_resource()), std::pmr::string("f", ruvia::process_resource())}, ruvia::process_resource())));
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{4});
    if (statements.size() == 4) {
        for (const auto& statement : statements) {
            EXPECT_TRUE(statement.sql_ == cached_sql);
        }
        EXPECT_TRUE((statements[0].parameter_types_ == std::pmr::vector<std::uint32_t>(1, 0, ruvia::process_resource())));
        EXPECT_TRUE((statements[0].parameter_values_ == parameter_values({std::pmr::string("1", ruvia::process_resource())}, ruvia::process_resource())));
        EXPECT_TRUE((statements[1].parameter_types_ == std::pmr::vector<std::uint32_t>(1, 0, ruvia::process_resource())));
        EXPECT_TRUE((statements[1].parameter_values_ == parameter_values({std::pmr::string("2", ruvia::process_resource()), std::pmr::string("3", ruvia::process_resource())}, ruvia::process_resource())));
        EXPECT_TRUE((statements[2].parameter_types_ == std::pmr::vector<std::uint32_t>({701}, ruvia::process_resource())));
        EXPECT_TRUE((statements[3].parameter_types_ == std::pmr::vector<std::uint32_t>({701}, ruvia::process_resource())));
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
            std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                    {.error_ = std::pmr::string("stale statement", ruvia::process_resource()), .sql_state_ = std::pmr::string(stale.sql_state_, ruvia::process_resource()), .routine_ = std::pmr::string(stale.routine_, ruvia::process_resource())},
                                                                                    {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}},
                ruvia::process_resource()));
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
        std::pmr::vector<std::pmr::string> values{ruvia::process_resource()};
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            auto handle = connected.handle();
            for (int use = 0; use < 3; ++use) {
                values.push_back(co_await first_value(handle, std::int64_t{1}));
            }
        });
        EXPECT_TRUE((values == std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource()), std::pmr::string("b", ruvia::process_resource()), std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())));
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("BEGIN", ruvia::process_resource()), .status_ = 'T'}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 'T'},
                                                                                {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource()), .status_ = 'T'},
                                                                                {.status_ = 'E', .error_ = std::pmr::string("cached plan must not change result type", ruvia::process_resource()), .sql_state_ = std::pmr::string("0A000", ruvia::process_resource()), .routine_ = std::pmr::string("RevalidateCachedQuery", ruvia::process_resource())},
                                                                                {.column_ = std::pmr::string("", ruvia::process_resource()), .command_ = std::pmr::string("ROLLBACK", ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}},
            ruvia::process_resource()));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::pmr::string sql_state{ruvia::process_resource()};
    bool transaction_failed = false;
    std::pmr::string after{ruvia::process_resource()};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
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
        std::pmr::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("a", ruvia::process_resource())}, ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("b", ruvia::process_resource())}, ruvia::process_resource())},
                                                                                {.error_ = std::pmr::string("not supported", ruvia::process_resource()), .sql_state_ = std::pmr::string("0A000", ruvia::process_resource()), .routine_ = std::pmr::string("ExecInitNode", ruvia::process_resource())}, {.rows_ = std::pmr::vector<std::pmr::string>({std::pmr::string("c", ruvia::process_resource())}, ruvia::process_resource())}},
            ruvia::process_resource()));
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::pmr::string after{ruvia::process_resource()};
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
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
