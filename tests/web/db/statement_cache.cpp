#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {

constexpr std::string_view cached_sql = "SELECT v FROM records WHERE id = $1";

template <typename param_type>
ruvia::task<std::string> first_value(ruvia::db_handle& handle, param_type param) {
    const ruvia::db_rows rows = co_await handle.query(cached_sql, param);
    co_return std::string(rows.front()["v"].as<std::string_view>().value_or(""));
}

using parameter_values = std::vector<std::optional<std::string>>;

}  // namespace

// A statement runs unnamed on its first use, is parsed as a named statement on
// its second, and is bound by name afterwards. The server fixes parameter
// types at Parse, so the same SQL with a differently typed parameter is a
// separate statement.
RUVIA_TEST(db_postgresql_repeated_statement_is_parsed_once_per_connection) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.rows_ = {"a"}}, {.rows_ = {"b"}}, {.rows_ = {"c"}}, {.rows_ = {"d"}}, {.rows_ = {"e"}},
            {.rows_ = {"f"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::vector<std::string> values;
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.handle();
        for (const std::int64_t id : {1, 2, 3}) {
            values.push_back(co_await first_value(handle, id));
        }
        for (const double id : {1.5, 2.5, 3.5}) {
            values.push_back(co_await first_value(handle, id));
        }
    });
    RUVIA_CHECK((values == std::vector<std::string>{"a", "b", "c", "d", "e", "f"}));
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{4});
    if (statements.size() == 4) {
        for (const auto& statement : statements) {
            RUVIA_CHECK(statement.sql_ == cached_sql);
        }
        RUVIA_CHECK((statements[0].parameter_types_ == std::vector<std::uint32_t>{0}));
        RUVIA_CHECK((statements[0].parameter_values_ == parameter_values{"1"}));
        RUVIA_CHECK((statements[1].parameter_types_ == std::vector<std::uint32_t>{0}));
        RUVIA_CHECK((statements[1].parameter_values_ == parameter_values{"2", "3"}));
        RUVIA_CHECK((statements[2].parameter_types_ == std::vector<std::uint32_t>{701}));
        RUVIA_CHECK((statements[3].parameter_types_ == std::vector<std::uint32_t>{701}));
        RUVIA_CHECK_EQ(statements[3].parameter_values_.size(), std::size_t{2});
    }
}

// Outside a transaction block, a cached statement the server lost (DEALLOCATE,
// DISCARD) or whose cached plan changed result type fails before it runs, so
// it runs once more without the caller seeing the failure.
RUVIA_TEST(db_postgresql_stale_cached_statement_is_retried) {
    using namespace ruvia;
    struct stale_case final {
        std::string_view sql_state_;
        std::string_view routine_;
    };
    for (const auto stale :
        {stale_case{"0A000", "RevalidateCachedQuery"}, stale_case{"26000", "FetchPreparedStatement"}}) {
        test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
            test::backend_startup_peer::after_startup::idle,
            {{.rows_ = {"a"}}, {.rows_ = {"b"}},
                {.error_ = "stale statement", .sql_state_ = std::string(stale.sql_state_), .routine_ = std::string(stale.routine_)},
                {.rows_ = {"c"}}});
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
        std::vector<std::string> values;
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            auto handle = connected.handle();
            for (int use = 0; use < 3; ++use) {
                values.push_back(co_await first_value(handle, std::int64_t{1}));
            }
        });
        RUVIA_CHECK((values == std::vector<std::string>{"a", "b", "c"}));
        // Unnamed, named, and the retry parsing the statement again.
        RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{3});
        RUVIA_CHECK_EQ(peer.startups(), std::size_t{1});
    }
}

// Inside a transaction block the failure has aborted the transaction, so it
// reaches the caller; the stale statement is parsed again on its next use.
RUVIA_TEST(db_postgresql_stale_statement_in_transaction_fails_and_is_parsed_again) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.column_ = "", .command_ = "BEGIN", .status_ = 'T'}, {.rows_ = {"a"}, .status_ = 'T'},
            {.rows_ = {"b"}, .status_ = 'T'},
            {.status_ = 'E', .error_ = "cached plan must not change result type", .sql_state_ = "0A000", .routine_ = "RevalidateCachedQuery"},
            {.column_ = "", .command_ = "ROLLBACK"}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::string sql_state;
    bool transaction_failed = false;
    std::string after;
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
    RUVIA_CHECK(sql_state == "0A000");
    RUVIA_CHECK(transaction_failed);
    RUVIA_CHECK(after == "c");
    const auto statements = peer.postgres_parses();
    RUVIA_CHECK_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        RUVIA_CHECK(statements[3].sql_ == "ROLLBACK");
        RUVIA_CHECK(statements[4].sql_ == cached_sql);
    }
}

// The same SQLSTATE raised while the statement runs is the statement's own
// failure: it is reported, not retried, and the statement is parsed again on
// its next use.
RUVIA_TEST(db_postgresql_stale_sql_state_raised_by_the_statement_is_not_retried) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        {{.rows_ = {"a"}}, {.rows_ = {"b"}},
            {.error_ = "not supported", .sql_state_ = "0A000", .routine_ = "ExecInitNode"}, {.rows_ = {"c"}}});
    auto& context_value = test::new_test_io_context();
    auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    std::optional<db_error::code_type> code;
    std::string after;
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
    RUVIA_CHECK(code == db_error::code_type::statement_failed);
    RUVIA_CHECK(after == "c");
    RUVIA_CHECK_EQ(peer.postgres_parses().size(), std::size_t{3});
}
#endif
