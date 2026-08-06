// Live MariaDB driver checks, skipped unless RUVIA_RUN_MARIADB_INTEGRATION=1.
//
// The centrepiece is the first case: MariaDB's asynchronous API suspends by
// yielding out of a fibre when the socket reports EAGAIN, so with a blocking
// socket -- which is what mysql_real_connect leaves behind -- mysql_*_start()
// runs the whole statement before returning, and the worker's event loop stops
// with it. Nothing above the driver can observe that from a unit test: the
// query still returns the right rows, just with every other connection on that
// worker frozen meanwhile. Here a timer ticks against a deliberately slow
// query, and silence means the loop was blocked.

#include "db_integration_fixture.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/detail/db/DbRegistry.h"

#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory_resource>
#include <span>
#include <string_view>

namespace {

[[nodiscard]] ruvia::DbConfig testConfig() {
    auto config = ruvia::DbConfig::mariaDb();
    config.host = ruvia::testing::dbEnvironment("RUVIA_TEST_MYSQL_HOST", "127.0.0.1");
    config.username = ruvia::testing::dbEnvironment("RUVIA_TEST_MYSQL_USER", "ruvia");
    config.password = ruvia::testing::dbEnvironment("RUVIA_TEST_MYSQL_PASSWORD", "ruvia");
    config.database = ruvia::testing::dbEnvironment("RUVIA_TEST_MYSQL_DATABASE", "ruvia");
    config.port = ruvia::testing::dbEnvironmentPort("RUVIA_TEST_MYSQL_PORT", "3306");
    config.connectTimeout = std::chrono::seconds(5);
    config.acquireTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(30);
    return config;
}

using ruvia::testing::dbRequire;
using ruvia::testing::dbThrowsOn;

void exerciseMigrations(const ruvia::DbConfig& config) {
    ruvia::DbMigrationOptions options;
    options.table = "ruvia_mariadb_integration_migrations";

    const std::array migrations{ruvia::DbMigration{"001_create_migrated",
        "CREATE TABLE IF NOT EXISTS ruvia_mariadb_integration_migrated ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, value VARCHAR(64) NOT NULL)"}};
    // The report owns its ids, so it is bound before they are read: the span
    // accessors are deleted on an rvalue for exactly that reason.
    const auto first = ruvia::DbMigrator::migrate(config, migrations, options);
    dbRequire(first.applied().size() == 1, "migration was not applied");
    const auto second = ruvia::DbMigrator::migrate(config, migrations, options);
    dbRequire(second.skipped().size() == 1, "migration was not idempotent");

    // Editing an applied migration changes nothing on a machine that already
    // ran it, so the edit is reported rather than skipped.
    const std::array edited{ruvia::DbMigration{"001_create_migrated",
        "CREATE TABLE IF NOT EXISTS ruvia_mariadb_integration_migrated ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, value VARCHAR(128) NOT NULL)"}};
    dbRequire(dbThrowsOn([&] { (void)ruvia::DbMigrator::migrate(config, edited, options); }), "an edited migration body was accepted");
}

ruvia::Task<void> exercise(asio::io_context& ioContext, unsigned& ticks) {
    auto* resource = std::pmr::get_default_resource();
    const std::array definitions{ruvia::detail::DbDefinition{std::pmr::string("default", resource), testConfig()}};
    ruvia::detail::DbRegistry registry(ioContext, resource, definitions);
    co_await registry.connect();
    ruvia::detail::ScopedOperationScope operationScope;
    auto db = registry.get(resource, operationScope);

    // The loop must keep running while the server takes its time.
    (void)co_await db.query("SELECT SLEEP(1)");
    dbRequire(ticks > 0, "the event loop was blocked for the duration of the query");

    (void)co_await db.execute("DROP TABLE IF EXISTS ruvia_mariadb_integration_items");
    (void)co_await db.execute("CREATE TABLE ruvia_mariadb_integration_items (id INT NOT NULL AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64) NOT NULL, n BIGINT NOT NULL)");

    const std::array<ruvia::DbValue, 2> item{ruvia::DbValue{std::string_view("a'b?c")}, ruvia::DbValue{std::int64_t{-42}}};
    auto inserted = co_await db.execute("INSERT INTO ruvia_mariadb_integration_items(name, n) VALUES (?, ?)", std::span<const ruvia::DbValue>(item));
    dbRequire(inserted.affectedRows() == 1, "insert affected-row count is incorrect");
    dbRequire(inserted.lastInsertId() == 1, "last insert id is incorrect");

    // '?' in the value is data; the one placeholder outside the literal takes
    // the parameter.
    auto typed = co_await db.query("SELECT name, n FROM ruvia_mariadb_integration_items WHERE name = ?", std::span<const ruvia::DbValue>(item.data(), 1));
    dbRequire(typed.rows().size() == 1 && typed.rows()[0].size() == 2, "typed query returned the wrong shape");
    dbRequire(typed.rows()[0][0].text() == "a'b?c", "text binding round trip failed");
    dbRequire(typed.rows()[0][1].text() == "-42", "integer binding round trip failed");

    // The same lookup with the parameters passed as ordinary arguments: both
    // bindings must reach the server in order and with their types intact, and
    // the '?' inside the value must still travel as data.
    auto variadic = co_await db.query("SELECT name, n FROM ruvia_mariadb_integration_items WHERE name = ? AND n = ?", std::string_view("a'b?c"), std::int64_t{-42});
    dbRequire(variadic.rows().size() == 1 && variadic.rows()[0].size() == 2, "variadic query returned the wrong shape");
    dbRequire(variadic.rows()[0][0].text() == "a'b?c", "variadic text binding round trip failed");
    dbRequire(variadic.rows()[0][1].text() == "-42", "variadic integer binding round trip failed");

    // The variadic write path, round-tripped so the row count the assertions
    // below depend on is unchanged.
    auto variadicUpdate = co_await db.execute("UPDATE ruvia_mariadb_integration_items SET n = ? WHERE name = ?", std::int64_t{-43}, std::string_view("a'b?c"));
    dbRequire(variadicUpdate.affectedRows() == 1, "variadic update affected-row count is incorrect");
    (void)co_await db.execute("UPDATE ruvia_mariadb_integration_items SET n = ? WHERE name = ?", std::int64_t{-42}, std::string_view("a'b?c"));

    // Larger than one socket read, so the async path has to suspend and resume
    // several times to assemble the result.
    (void)co_await db.execute("INSERT INTO ruvia_mariadb_integration_items(name, n) SELECT CONCAT('bulk-', seq), seq FROM seq_1_to_2000");
    auto bulk = co_await db.query("SELECT id, name, n FROM ruvia_mariadb_integration_items ORDER BY id");
    dbRequire(bulk.rows().size() == 2001, "bulk result is missing rows");
    dbRequire(bulk.rows()[2000][2].text() == "2000", "bulk result ends on the wrong row");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_mariadb_integration_items(name, n) VALUES ('rolled', 1)");
        co_await transaction.rollback();
    }
    auto afterRollback = co_await db.query("SELECT count(*) FROM ruvia_mariadb_integration_items");
    dbRequire(afterRollback.rows()[0][0].text() == "2001", "rollback did not restore state");

    {
        auto transaction = co_await db.beginTransaction();
        (void)co_await transaction.execute("INSERT INTO ruvia_mariadb_integration_items(name, n) VALUES ('kept', 2)");
        co_await transaction.commit();
    }
    auto afterCommit = co_await db.query("SELECT count(*) FROM ruvia_mariadb_integration_items");
    dbRequire(afterCommit.rows()[0][0].text() == "2002", "commit did not persist state");

    auto stream = co_await db.queryStream("SELECT id FROM ruvia_mariadb_integration_items ORDER BY id");
    std::size_t streamed = 0;
    while (auto row = co_await stream.read()) {
        dbRequire(row->size() == 1, "streamed row has the wrong shape");
        ++streamed;
    }
    dbRequire(streamed == 2002, "the stream did not deliver every row");

    (void)co_await db.execute("DROP TABLE ruvia_mariadb_integration_items");
    (void)co_await db.execute("DROP TABLE IF EXISTS ruvia_mariadb_integration_migrated");
    (void)co_await db.execute("DROP TABLE IF EXISTS ruvia_mariadb_integration_migrations");
    registry.closeNow();
}

}  // namespace

int main() {
    if (!ruvia::testing::dbIntegrationRequested("RUVIA_RUN_MARIADB_INTEGRATION")) {
        std::puts(
            "MariaDB integration skipped; set "
            "RUVIA_RUN_MARIADB_INTEGRATION=1 to run it");
        return 77;
    }

    try {
        exerciseMigrations(testConfig());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MariaDB integration failed: %s\n", error.what());
        return 1;
    }

    asio::io_context ioContext(1);
    unsigned ticks = 0;
    asio::steady_timer heartbeat(ioContext);
    // Re-armed from its own completion, which is what makes a silent stretch
    // measurable: a blocked loop simply stops delivering it. Cancelling is not
    // enough to end it -- a tick already queued when the work finished would
    // re-arm and keep run() alive forever -- so the flag is what stops the
    // chain and the cancel only wakes the pending wait.
    struct Heartbeat final {
        asio::steady_timer& timer;
        unsigned& ticks;
        bool& stopped;

        void operator()(const std::error_code& error) const {
            if (error || stopped) {
                return;
            }
            ++ticks;
            timer.expires_after(std::chrono::milliseconds(20));
            timer.async_wait(*this);
        }
    };
    bool stopped = false;
    heartbeat.expires_after(std::chrono::milliseconds(20));
    heartbeat.async_wait(Heartbeat{heartbeat, ticks, stopped});

    std::exception_ptr failure;
    ruvia::detail::asyncStartTask(exercise(ioContext, ticks), asio::bind_executor(ioContext.get_executor(), [&failure, &stopped, &heartbeat](ruvia::detail::TaskCompletionResult<void> result) {
        if (const auto* error = result.failure()) {
            failure = error->exception();
        }
        stopped = true;
        heartbeat.cancel();
    }));
    ioContext.run();

    if (failure != nullptr) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "MariaDB integration failed: %s\n", error.what());
            return 1;
        }
    }
    std::printf("MariaDB integration passed (%u event-loop ticks)\n", ticks);
    return 0;
}
