#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbRegistry.h"

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kItemsTable = "ruvia_pg_integration_items";
constexpr std::string_view kMigrationsTable = "ruvia_pg_integration_migrations";

[[nodiscard]] std::string_view environment(const char* name, std::string_view fallback) noexcept {
    const auto* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string_view(value) : fallback;
}

[[nodiscard]] ruvia::DbConfig testConfig() {
    auto config = ruvia::DbConfig::postgreSql();
    config.host = environment("RUVIA_TEST_PG_HOST", "127.0.0.1");
    config.username = environment("RUVIA_TEST_PG_USER", "ruvia");
    config.password = environment("RUVIA_TEST_PG_PASSWORD", "ruvia");
    config.database = environment("RUVIA_TEST_PG_DATABASE", "ruvia");
    const auto port = environment("RUVIA_TEST_PG_PORT", "55432");
    unsigned parsedPort = 0;
    for (const auto character : port) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument("RUVIA_TEST_PG_PORT must be numeric");
        }
        parsedPort = parsedPort * 10U + static_cast<unsigned>(character - '0');
    }
    if (parsedPort == 0 || parsedPort > 65535) {
        throw std::invalid_argument("RUVIA_TEST_PG_PORT is outside the valid range");
    }
    config.port = static_cast<std::uint16_t>(parsedPort);
    config.acquireTimeout = std::chrono::seconds(5);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(5);
    return config;
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Factory>
void runTask(Factory&& factory) {
    asio::io_context ioContext(1);
    std::exception_ptr exception;
    ruvia::detail::asyncStartTask(factory(ioContext), asio::bind_executor(ioContext.get_executor(), [&exception](ruvia::detail::TaskCompletionResult<void> result) {
        if (const auto* failure = result.failure()) {
            exception = failure->exception();
        }
    }));
    ioContext.run();
    if (exception != nullptr) {
        std::rethrow_exception(exception);
    }
}

ruvia::Task<void> withDatabase(asio::io_context& ioContext, ruvia::DbConfig config, bool cleanupOnly) {
    auto* resource = std::pmr::get_default_resource();
    const std::array definitions{ruvia::detail::DbDefinition{std::pmr::string("default", resource), config}};
    ruvia::detail::DbRegistry registry(ioContext, resource, definitions);
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
    require(typed.rows().size() == 1 && typed.rows()[0].size() == 4, "typed PostgreSQL query returned the wrong shape");
    require(typed.rows()[0][0].text() == "hello", "text binding failed");
    require(typed.rows()[0][1].text() == "-42", "integer binding failed");
    require(typed.rows()[0][2].text() == "t", "boolean binding failed");
    require(typed.rows()[0][3].text() == "t", "NULL binding failed");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_pg_integration_items(value) VALUES ($1)", std::span<const ruvia::DbValue>(params.data(), 1));
        co_await transaction.rollback();
    }
    auto count = co_await db.query("SELECT count(*) FROM ruvia_pg_integration_items");
    require(count.rows()[0][0].text() == "0", "rollback did not restore state");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_pg_integration_items(value) VALUES ($1)", std::span<const ruvia::DbValue>(params.data(), 1));
        co_await transaction.commit();
    }
    auto committedCount = co_await db.query("SELECT count(*) FROM ruvia_pg_integration_items");
    require(committedCount.rows()[0][0].text() == "1", "commit did not persist state");
    auto updated = co_await db.execute("UPDATE ruvia_pg_integration_items SET value = $1", std::span<const ruvia::DbValue>(params.data(), 1));
    require(updated.affectedRows() == 1, "affected-row count is incorrect");

    auto stream = co_await db.queryStream("SELECT generate_series(1, 128)");
    std::size_t streamed = 0;
    while (auto row = co_await stream.read()) {
        require(row->size() == 1, "streamed row has the wrong shape");
        ++streamed;
    }
    require(streamed == 128, "single-row mode did not stream every row");

    auto abandoned = co_await db.queryStream("SELECT generate_series(1, 128)");
    require((co_await abandoned.read()).has_value(), "stream produced no first row");
    co_await abandoned.close();
    auto reconnected = co_await db.query("SELECT 1");
    require(reconnected.rows()[0][0].text() == "1", "pool did not reconnect after an abandoned stream");
    registry.closeNow();
}

}  // namespace

int main() {
    const auto* runIntegration = std::getenv("RUVIA_RUN_POSTGRESQL_INTEGRATION");
    if (runIntegration == nullptr || std::string_view(runIntegration) != "1") {
        std::puts(
            "PostgreSQL integration skipped; set "
            "RUVIA_RUN_POSTGRESQL_INTEGRATION=1 to run it");
        return 77;
    }

    try {
        const auto config = testConfig();
        runTask([&](asio::io_context& ioContext) { return withDatabase(ioContext, config, true); });

        const std::array migrations{ruvia::DbMigration{"001_create_items",
            "CREATE TABLE ruvia_pg_integration_items ("
            "id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY, "
            "value TEXT NOT NULL)"}};
        ruvia::DbMigrationOptions options;
        options.table = kMigrationsTable;
        const auto first = ruvia::DbMigrator::migrate(config, migrations, options);
        const auto second = ruvia::DbMigrator::migrate(config, migrations, options);
        require(first.applied().size() == 1, "migration was not applied");
        require(second.skipped().size() == 1, "migration was not idempotent");

        runTask([&](asio::io_context& ioContext) { return withDatabase(ioContext, config, false); });
        runTask([&](asio::io_context& ioContext) { return withDatabase(ioContext, config, true); });
        std::puts("PostgreSQL integration passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PostgreSQL integration failed: %s\n", error.what());
        return 1;
    }
}
