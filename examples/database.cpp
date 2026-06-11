#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/app/App.h"
#include "ruvia/db/Db.h"
#include "ruvia/http/Controller.h"

namespace {

void assignIfPresent(std::pmr::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

ruvia::DbConfig dbConfigFromEnv(const ruvia::Env& env) {
    ruvia::DbConfig config;
    assignIfPresent(config.host, env.get("RUVIA_DB_HOST"));
    assignIfPresent(config.username, env.get("RUVIA_DB_USER"));
    assignIfPresent(config.password, env.get("RUVIA_DB_PASSWORD"));
    assignIfPresent(config.database, env.get("RUVIA_DB_DATABASE"));
    if (const auto port = env.get<std::uint16_t>("RUVIA_DB_PORT")) {
        config.port = *port;
    }
    if (const auto poolSize = env.get<std::uint32_t>("RUVIA_DB_POOL_SIZE")) {
        config.poolSize = *poolSize;
    }
    config.acquireTimeout = std::chrono::seconds(2);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(30);
    return config;
}

}  // namespace

class DatabaseController final : public ruvia::Controller<DatabaseController> {
public:
    RUVIA_CONTROLLER_GROUP("/db")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/users/:id", findUser);
    RUVIA_GET("/users", streamUsers);
    RUVIA_POST("/users", createUser);
    RUVIA_POST("/transfer", transfer);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> findUser(ruvia::Context& c) {
        auto result = co_await c.db().query(
            "SELECT id, name FROM users WHERE id = ?",
            {c.param("id").toStringView().value_or("")});

        std::pmr::string body(c.allocator<char>());
        body.append(result.rows().empty() ? "not found\n" : "found\n");
        co_return c.text(body, result.rows().empty() ? 404 : 200);
    }

    ruvia::Task<ruvia::HttpResponse> streamUsers(ruvia::Context& c) {
        auto rows = co_await c.db().queryStream("SELECT name FROM users ORDER BY id");
        std::pmr::string body(c.allocator<char>());
        while (auto row = co_await rows.read()) {
            if (!row->empty()) {
                body.append((*row)[0].text());
                body.push_back('\n');
            }
        }
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> createUser(ruvia::Context& c) {
        const auto name = co_await c.body();
        auto result = co_await c.db().execute(
            "INSERT INTO users(name) VALUES (?)",
            {name});
        std::pmr::string body(c.allocator<char>());
        body.append("created id=");
        appendUnsigned(body, result.lastInsertId());
        body.push_back('\n');
        co_return c.text(body, 201);
    }

    ruvia::Task<ruvia::HttpResponse> transfer(ruvia::Context& c) {
        auto tx = co_await c.db().beginTransaction();
        auto debit = co_await tx.execute("UPDATE accounts SET balance = balance - ? WHERE id = ?", {100, 1});
        auto credit = co_await tx.execute("UPDATE accounts SET balance = balance + ? WHERE id = ?", {100, 2});
        (void)debit;
        (void)credit;
        co_await tx.commit();
        co_return c.text("transfer committed\n");
    }

    static void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }
};

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();

    const auto config = dbConfigFromEnv(app.env());
    if (!config.username.empty() && !config.database.empty()) {
        static constexpr std::array migrations{
            ruvia::DbMigration{
                "001_create_users",
                "CREATE TABLE IF NOT EXISTS users ("
                "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                "name VARCHAR(120) NOT NULL)"
            },
            ruvia::DbMigration{
                "002_create_accounts",
                "CREATE TABLE IF NOT EXISTS accounts ("
                "id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
                "balance BIGINT NOT NULL)"
            },
        };

        if (app.env().get<bool>("RUVIA_DB_MIGRATE").value_or(false)) {
            (void)ruvia::DbMigrator::migrate(config, migrations);
        }
        app.useDb(config);
    }

    app
        .setListenAddress("0.0.0.0", 8086)
        .setThreadNum(2)
        .run();
}
