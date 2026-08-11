#include "db_integration_fixture.h"

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbRegistry.h"

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::string_view kMigrationsTable = "ruvia_pg_integration_migrations";

[[nodiscard]] ruvia::DbConfig testConfig() {
    auto config = ruvia::DbConfig::postgreSql();
    config.host = ruvia::testing::dbEnvironment("RUVIA_TEST_PG_HOST", "127.0.0.1");
    config.username = ruvia::testing::dbEnvironment("RUVIA_TEST_PG_USER", "ruvia");
    config.password = ruvia::testing::dbEnvironment("RUVIA_TEST_PG_PASSWORD", "ruvia");
    config.database = ruvia::testing::dbEnvironment("RUVIA_TEST_PG_DATABASE", "ruvia");
    config.port = ruvia::testing::dbEnvironmentPort("RUVIA_TEST_PG_PORT", "55432");
    config.acquireTimeout = std::chrono::seconds(5);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(5);
    return config;
}

using ruvia::testing::dbRequire;
using ruvia::testing::dbThrowsOn;

template <typename Factory>
void runTask(Factory&& factory) {
    asio::io_context ioContext(1);
    auto attachment = ruvia::attachEventLoop(ioContext, {.mailboxCapacity = 64});
    const auto worker = attachment.loop().handle();
    std::exception_ptr exception;
    ruvia::detail::asyncStartTask(factory(ioContext, worker), asio::bind_executor(ioContext.get_executor(), [&exception, &attachment](ruvia::detail::TaskCompletionResult<void> result) {
        if (const auto* failure = result.failure()) {
            exception = failure->exception();
        }
        attachment.stop();
    }));
    ioContext.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
}

ruvia::Task<void> withDatabase(
    asio::io_context& ioContext,
    const ruvia::WorkerHandle& worker,
    ruvia::DbConfig config,
    bool cleanupOnly) {
    auto* resource = std::pmr::get_default_resource();
    const std::array definitions{ruvia::detail::DbDefinition{std::pmr::string("default", resource), ruvia::detail::DbConfigStorage(config, resource)}};
    ruvia::detail::DbRegistry registry(ioContext, resource, definitions, &worker);
    co_await registry.connect();
    ruvia::detail::ScopedOperationScope operationScope;
    auto db = registry.get(resource, operationScope);

    if (cleanupOnly) {
        (void)co_await db.execute("DROP TABLE IF EXISTS ruvia_pg_integration_items");
        (void)co_await db.execute("DROP TABLE IF EXISTS ruvia_pg_integration_migrations");
        registry.closeNow();
        co_return;
    }

    const std::array<ruvia::DbValue, 4> params{ruvia::DbValue{"hello"}, ruvia::DbValue{-42}, ruvia::DbValue{true}, ruvia::DbValue{nullptr}};
    auto typed = co_await db.query("SELECT $1::text, $2::bigint, $3::boolean, $4::text IS NULL", std::span<const ruvia::DbValue>(params));
    dbRequire(typed.rows().size() == 1 && typed.rows()[0].size() == 4, "typed PostgreSQL query returned the wrong shape");
    dbRequire(typed.rows()[0][0].text() == "hello", "text binding failed");
    dbRequire(typed.rows()[0][1].text() == "-42", "integer binding failed");
    dbRequire(typed.rows()[0][2].text() == "t", "boolean binding failed");
    dbRequire(typed.rows()[0][3].text() == "t", "NULL binding failed");

    {
        auto stop = std::make_shared<ruvia::detail::StopSource>();
        asio::steady_timer cancel(ioContext, std::chrono::milliseconds(50));
        cancel.async_wait([stop](std::error_code error) {
            if (!error) {
                stop->requestStop();
            }
        });
        bool cancelled = false;
        try {
            (void)co_await db.withOptions({.stopToken = stop->token()}).query("SELECT pg_sleep(5)");
        } catch (const ruvia::DbError& error) {
            cancelled = error.code() == ruvia::DbError::Code::kCancelled;
        }
        dbRequire(cancelled, "active PostgreSQL query did not report kCancelled");
        auto recovered = co_await db.query("SELECT 1");
        dbRequire(recovered.rows()[0][0].text() == "1", "PostgreSQL did not reconnect after query cancellation");
    }

    {
        auto stop = std::make_shared<ruvia::detail::StopSource>();
        auto transaction = co_await db.withOptions({.stopToken = stop->token()}).beginTransaction();
        asio::steady_timer cancel(ioContext, std::chrono::milliseconds(50));
        cancel.async_wait([stop](std::error_code error) {
            if (!error) {
                stop->requestStop();
            }
        });
        bool cancelled = false;
        try {
            (void)co_await transaction.query("SELECT pg_sleep(5)");
        } catch (const ruvia::DbError& error) {
            cancelled = error.code() == ruvia::DbError::Code::kCancelled;
        }
        dbRequire(cancelled && !transaction.active(), "active PostgreSQL transaction did not fail with kCancelled");
        auto recovered = co_await db.query("SELECT 1");
        dbRequire(recovered.rows()[0][0].text() == "1", "PostgreSQL did not reconnect after transaction cancellation");
    }

    {
        auto stop = std::make_shared<ruvia::detail::StopSource>();
        auto stream = co_await db.withOptions({.stopToken = stop->token()})
                          .queryStream("SELECT i, pg_sleep(0.01) FROM generate_series(1, 1000) AS i");
        asio::steady_timer cancel(ioContext, std::chrono::milliseconds(50));
        cancel.async_wait([stop](std::error_code error) {
            if (!error) {
                stop->requestStop();
            }
        });
        bool cancelled = false;
        try {
            while (co_await stream.read()) {
            }
        } catch (const ruvia::DbError& error) {
            cancelled = error.code() == ruvia::DbError::Code::kCancelled;
        }
        dbRequire(cancelled && !stream.active(), "active PostgreSQL stream did not fail with kCancelled");
        auto recovered = co_await db.query("SELECT 1");
        dbRequire(recovered.rows()[0][0].text() == "1", "PostgreSQL did not reconnect after stream cancellation");
    }

    // The same bindings passed as ordinary arguments must reach the server in
    // the same order and with the same types as the prepared span above.
    auto variadic = co_await db.query("SELECT $1::text, $2::bigint, $3::boolean, $4::text IS NULL", "hello", -42, true, nullptr);
    dbRequire(variadic.rows().size() == 1 && variadic.rows()[0].size() == 4, "variadic PostgreSQL query returned the wrong shape");
    dbRequire(variadic.rows()[0][0].text() == "hello", "variadic text binding failed");
    dbRequire(variadic.rows()[0][1].text() == "-42", "variadic integer binding failed");
    dbRequire(variadic.rows()[0][2].text() == "t", "variadic boolean binding failed");
    dbRequire(variadic.rows()[0][3].text() == "t", "variadic NULL binding failed");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_pg_integration_items(value) VALUES ($1)", std::span<const ruvia::DbValue>(params.data(), 1));
        co_await transaction.rollback();
    }
    auto count = co_await db.query("SELECT count(*) FROM ruvia_pg_integration_items");
    dbRequire(count.rows()[0][0].text() == "0", "rollback did not restore state");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_pg_integration_items(value) VALUES ($1)", std::span<const ruvia::DbValue>(params.data(), 1));
        co_await transaction.commit();
    }
    auto committedCount = co_await db.query("SELECT count(*) FROM ruvia_pg_integration_items");
    dbRequire(committedCount.rows()[0][0].text() == "1", "commit did not persist state");
    auto updated = co_await db.execute("UPDATE ruvia_pg_integration_items SET value = $1", std::span<const ruvia::DbValue>(params.data(), 1));
    dbRequire(updated.affectedRows() == 1, "affected-row count is incorrect");

    bool rejectedCommandStream = false;
    try {
        auto commandStream = co_await db.queryStream("UPDATE ruvia_pg_integration_items SET value = value WHERE value = 'missing'");
        (void)co_await commandStream.read();
    } catch (const std::invalid_argument&) {
        rejectedCommandStream = true;
    }
    dbRequire(rejectedCommandStream, "queryStream accepted non-row-producing SQL");
    auto afterRejectedStream = co_await db.query("SELECT 1");
    dbRequire(afterRejectedStream.rows()[0][0].text() == "1", "pool was not reusable after a rejected stream query");

    auto stream = co_await db.queryStream("SELECT generate_series(1, 128)");
    std::size_t streamed = 0;
    while (auto row = co_await stream.read()) {
        dbRequire(row->size() == 1, "streamed row has the wrong shape");
        ++streamed;
    }
    dbRequire(streamed == 128, "single-row mode did not stream every row");

    auto abandoned = co_await db.queryStream("SELECT generate_series(1, 128)");
    dbRequire((co_await abandoned.read()).has_value(), "stream produced no first row");
    co_await abandoned.close();
    auto reconnected = co_await db.query("SELECT 1");
    dbRequire(reconnected.rows()[0][0].text() == "1", "pool did not reconnect after an abandoned stream");
    registry.closeNow();
}

}  // namespace

int main() {
    if (!ruvia::testing::dbIntegrationRequested("RUVIA_RUN_POSTGRESQL_INTEGRATION")) {
        std::puts(
            "PostgreSQL integration skipped; set "
            "RUVIA_RUN_POSTGRESQL_INTEGRATION=1 to run it");
        return 77;
    }

    try {
        const auto config = testConfig();
        runTask([&](asio::io_context& ioContext, const ruvia::WorkerHandle& worker) { return withDatabase(ioContext, worker, config, true); });

        const std::array migrations{ruvia::DbMigration{"001_create_items",
            "CREATE TABLE ruvia_pg_integration_items ("
            "id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
            "value TEXT NOT NULL)"}};
        ruvia::DbMigrationOptions options;
        options.table = kMigrationsTable;
        const auto first = ruvia::DbMigrator::migrate(config, migrations, options);
        const auto second = ruvia::DbMigrator::migrate(config, migrations, options);
        dbRequire(first.applied().size() == 1, "migration was not applied");
        dbRequire(second.skipped().size() == 1, "migration was not idempotent");

        // Editing an applied migration changes nothing on a machine that
        // already ran it, so the edit is reported rather than skipped.
        const std::array edited{ruvia::DbMigration{"001_create_items",
            "CREATE TABLE ruvia_pg_integration_items ("
            "id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
            "value TEXT NOT NULL, note TEXT)"}};
        dbRequire(dbThrowsOn([&] { (void)ruvia::DbMigrator::migrate(config, edited, options); }),
            "an edited migration body was accepted");

        // CREATE INDEX CONCURRENTLY is refused inside a transaction block, so
        // it reports whether the default wrap is really there -- and whether
        // naming the exception really lifts it.
        const std::array wrapped{ruvia::DbMigration{"002_concurrent_index",
            "CREATE INDEX CONCURRENTLY ruvia_pg_integration_items_value_idx "
            "ON ruvia_pg_integration_items (value)"}};
        dbRequire(dbThrowsOn([&] { (void)ruvia::DbMigrator::migrate(config, wrapped, options); }),
            "a transactional migration did not run inside a transaction block");

        const std::array unwrapped{ruvia::DbMigration{"002_concurrent_index",
            "CREATE INDEX CONCURRENTLY ruvia_pg_integration_items_value_idx "
            "ON ruvia_pg_integration_items (value)",
            ruvia::DbMigrationAtomicity::kUnwrapped}};
        const auto concurrent = ruvia::DbMigrator::migrate(config, unwrapped, options);
        dbRequire(concurrent.applied().size() == 1,
            "an unwrapped migration was not applied outside a transaction block");

        runTask([&](asio::io_context& ioContext, const ruvia::WorkerHandle& worker) { return withDatabase(ioContext, worker, config, false); });
        runTask([&](asio::io_context& ioContext, const ruvia::WorkerHandle& worker) { return withDatabase(ioContext, worker, config, true); });
        std::puts("PostgreSQL integration passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PostgreSQL integration failed: %s\n", error.what());
        return 1;
    }
}
