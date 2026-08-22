// Database: unified MariaDB/PostgreSQL configuration, query, execute,
// streaming query, transaction and optional migration. Built with either
// database feature.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/Controller.h"

namespace {

void assignIfPresent(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

ruvia::DbConfig dbConfigFromEnv(const ruvia::Env& env) {
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    const auto driver = env.get("RUVIA_DB_DRIVER");
    auto config = driver && *driver == "postgresql" ? ruvia::DbConfig::postgreSql() : ruvia::DbConfig::mariaDb();
#elif defined(RUVIA_ENABLE_MARIADB)
    auto config = ruvia::DbConfig::mariaDb();
#else
    auto config = ruvia::DbConfig::postgreSql();
#endif
    assignIfPresent(config.host, env.get("RUVIA_DB_HOST"));
    assignIfPresent(config.username, env.get("RUVIA_DB_USER"));
    assignIfPresent(config.password, env.get("RUVIA_DB_PASSWORD"));
    assignIfPresent(config.database, env.get("RUVIA_DB_DATABASE"));
    if (const auto port = env.get<std::uint16_t>("RUVIA_DB_PORT")) {
        config.port = *port;
    }
    config.acquireTimeout = std::chrono::seconds(2);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(30);
    return config;
}

}  // namespace

class DatabaseController final : public ruvia::Controller<DatabaseController> {
public:
    static void setDriver(ruvia::DbDriver driver) noexcept {
        driver_ = driver;
    }

    RUVIA_CONTROLLER_GROUP("/db")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/users/:id", findUser);
    RUVIA_GET("/users", streamUsers);
    RUVIA_POST("/users", createUser);
    RUVIA_POST("/transfer", transfer);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> findUser(ruvia::Context& c) {
        bool found = false;
        co_await loadUserFound(c, found);
        std::pmr::string body(c.allocator<char>());
        body.append(found ? "found\n" : "not found\n");
        c.status(found ? ruvia::http_status::kOk : ruvia::http_status::kNotFound);
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> streamUsers(ruvia::Context& c) {
        std::pmr::string body(c.allocator<char>());
        co_await appendUsers(c, body);
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> createUser(ruvia::Context& c) {
        const auto name = co_await c.req().text();
        std::uint64_t id = 0;
        co_await insertUser(c, name, id);
        std::pmr::string body(c.allocator<char>());
        body.append("created id=");
        appendUnsigned(body, id);
        body.push_back('\n');
        c.status(ruvia::http_status::kCreated);
        co_return c.text(std::move(body));
    }

    ruvia::Task<ruvia::HttpResponse> transfer(ruvia::Context& c) {
        co_await transferFunds(c);
        co_return c.text("transfer committed\n");
    }

    static ruvia::Task<void> loadUserFound(ruvia::Context& c, bool& found) {
        auto result = co_await c.db().query(driver_ == ruvia::DbDriver::kPostgreSql ? "SELECT id, name FROM users WHERE id = $1" : "SELECT id, name FROM users WHERE id = ?", c.req().param("id").value_or(""));
        found = !result.empty();
        co_return;
    }

    static ruvia::Task<void> appendUsers(ruvia::Context& c, std::pmr::string& body) {
        auto rows = co_await c.db().queryStream("SELECT name FROM users ORDER BY id");
        while (auto row = co_await rows.read()) {
            if (!row->empty()) {
                body.append((*row)["name"].value().value_or(""));
                body.push_back('\n');
            }
        }
        co_return;
    }

    static ruvia::Task<void> insertUser(ruvia::Context& c, std::string_view name, std::uint64_t& id) {
        if (driver_ == ruvia::DbDriver::kPostgreSql) {
            auto result = co_await c.db().query("INSERT INTO users(name) VALUES ($1) RETURNING id", name);
            if (result.empty() || result.front().empty()) {
                throw std::runtime_error("PostgreSQL INSERT did not return an id");
            }
            id = result.front()["id"].as<std::uint64_t>().value();
        } else {
            const auto result = co_await c.db().execute("INSERT INTO users(name) VALUES (?)", name);
            id = result.lastInsertId().value_or(0);
        }
        co_return;
    }

    static ruvia::Task<void> transferFunds(ruvia::Context& c) {
        auto tx = co_await c.db().beginTransaction();
        (void)co_await tx.execute(driver_ == ruvia::DbDriver::kPostgreSql ? "UPDATE accounts SET balance = balance - $1 WHERE id = $2" : "UPDATE accounts SET balance = balance - ? WHERE id = ?", 100, 1);
        (void)co_await tx.execute(driver_ == ruvia::DbDriver::kPostgreSql ? "UPDATE accounts SET balance = balance + $1 WHERE id = $2" : "UPDATE accounts SET balance = balance + ? WHERE id = ?", 100, 2);
        co_await tx.commit();
        co_return;
    }

    static void appendUnsigned(std::pmr::string& output, std::uint64_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }

    static inline ruvia::DbDriver driver_{ruvia::DbDriver::kMariaDb};
};

int main() {
    auto& app = ruvia::app();
    app.loadDotenv();

    const auto config = dbConfigFromEnv(app.env());
    if (!config.username.empty() && !config.database.empty()) {
        static constexpr std::array mariaDbMigrations{
            ruvia::DbMigration{{.id = "001_create_users",
                .sql = "CREATE TABLE IF NOT EXISTS users ("
                       "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                       "name VARCHAR(120) NOT NULL)"}},
            ruvia::DbMigration{{.id = "002_create_accounts",
                .sql = "CREATE TABLE IF NOT EXISTS accounts ("
                       "id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
                       "balance BIGINT NOT NULL)"}},
        };
        static constexpr std::array postgreSqlMigrations{
            ruvia::DbMigration{{.id = "001_create_users",
                .sql = "CREATE TABLE IF NOT EXISTS users ("
                       "id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
                       "name VARCHAR(120) NOT NULL)"}},
            ruvia::DbMigration{{.id = "002_create_accounts",
                .sql = "CREATE TABLE IF NOT EXISTS accounts ("
                       "id BIGINT PRIMARY KEY,"
                       "balance BIGINT NOT NULL)"}},
        };

        if (app.env().get<bool>("RUVIA_DB_MIGRATE").value_or(false)) {
            if (config.driver() == ruvia::DbDriver::kPostgreSql) {
                (void)ruvia::DbMigrator::migrate(config, postgreSqlMigrations);
            } else {
                (void)ruvia::DbMigrator::migrate(config, mariaDbMigrations);
            }
        }
        DatabaseController::setDriver(config.driver());
        app.useDb({.config = config});
    }

    app.setListeners({ruvia::ListenerConfig::http({.address = "0.0.0.0", .port = 8086})}).setWorkersPerListener(2).setProcessSignalHandlers(ruvia::ProcessSignalHandlerPolicy::kInstall).run();
}
