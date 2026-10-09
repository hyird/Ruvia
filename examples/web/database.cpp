// Database: unified MariaDB/PostgreSQL configuration, query, execute,
// streaming query, transaction and optional migration. Built with either
// database feature.

// Direct SQL, buffered/streamed rows, transactions, and startup migrations.
// Build with a SQL backend and set RUVIA_DB_DRIVER=postgresql or mariadb,
// RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE. Run on port 8086.
// RUVIA_DB_MIGRATE=true explicitly enables the demo schema migrations.
// POST a name to /db/users, then GET /db/users or /db/users/1.
// /db/transfer expects accounts 1 and 2 to exist; it commits both updates
// together. An uncommitted transaction rolls back when released.
// Use a dedicated demo database: writes and migration history persist.
// See backend_tls.h for RUVIA_DB_TLS/CA/CERT/KEY. Local plaintext servers
// require the explicit setting RUVIA_DB_TLS=false.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/db/db.h"

#include "backend_tls.h"

namespace {

void assign_if_present(std::string& target, std::optional<std::string_view> value) {
    if (value) {
        target.assign(value->data(), value->size());
    }
}

ruvia::db_config db_config_from_env(const example::environment& env_value) {
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    const auto driver = env_value.get("RUVIA_DB_DRIVER");
    auto config = driver && *driver == "postgresql"
                      ? ruvia::db_config{.driver_ = ruvia::db_driver::postgresql}
                      : ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#elif defined(RUVIA_ENABLE_MARIADB)
    auto config = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#else
    auto config = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
    assign_if_present(config.host_, env_value.get("RUVIA_DB_HOST"));
    assign_if_present(config.username_, env_value.get("RUVIA_DB_USER"));
    assign_if_present(config.password_, env_value.get("RUVIA_DB_PASSWORD"));
    assign_if_present(config.database_, env_value.get("RUVIA_DB_DATABASE"));
    config.tls_ = example::backend_tls("RUVIA_DB", env_value);
    if (const auto port = env_value.get<std::uint16_t>("RUVIA_DB_PORT")) {
        config.port_ = *port;
    }
    config.acquire_timeout_ = std::chrono::seconds(2);
    config.connect_timeout_ = std::chrono::seconds(5);
    config.query_timeout_ = std::chrono::seconds(30);
    return config;
}

}  // namespace

class database_controller final : public ruvia::controller<database_controller> {
public:
    static void set_driver(ruvia::db_driver driver) noexcept {
        driver_ = driver;
    }

    RUVIA_CONTROLLER_GROUP("/db")

    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/users/:id", find_user);
    RUVIA_GET("/users", stream_users);
    RUVIA_POST("/users", create_user);
    RUVIA_POST("/transfer", transfer);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> find_user(ruvia::context& c) {
        bool found = false;
        co_await load_user_found(c, found);
        std::pmr::string body(c.allocator<char>());
        body.append(found ? "found\n" : "not found\n");
        c.status(found ? ruvia::http_status::ok : ruvia::http_status::not_found);
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> stream_users(ruvia::context& c) {
        std::pmr::string body(c.allocator<char>());
        co_await append_users(c, body);
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> create_user(ruvia::context& c) {
        const auto name = co_await c.req().text();
        std::uint64_t id = 0;
        co_await insert_user(c, name, id);
        std::pmr::string body(c.allocator<char>());
        body.append("created id=");
        append_unsigned(body, id);
        body.push_back('\n');
        c.status(ruvia::http_status::created);
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> transfer(ruvia::context& c) {
        co_await transfer_funds(c);
        co_return c.text("transfer committed\n");
    }

    static ruvia::task<void> load_user_found(ruvia::context& c, bool& found) {
        auto result_value = co_await c.db().query(driver_ == ruvia::db_driver::postgresql
                                                      ? "SELECT id, name FROM users WHERE id = $1"
                                                      : "SELECT id, name FROM users WHERE id = ?",
            c.req().param("id").value_or(""));
        found = !result_value.empty();
        co_return;
    }

    static ruvia::task<void> append_users(ruvia::context& c, std::pmr::string& body) {
        auto rows = co_await c.db().query_stream("SELECT name FROM users ORDER BY id");
        while (auto row = co_await rows.read()) {
            if (!row->empty()) {
                body.append((*row)["name"].value().value_or(""));
                body.push_back('\n');
            }
        }
        co_return;
    }

    static ruvia::task<void> insert_user(
        ruvia::context& c, std::string_view name, std::uint64_t& id) {
        if (driver_ == ruvia::db_driver::postgresql) {
            auto result_value =
                co_await c.db().query<"INSERT INTO users(name) VALUES ($1) RETURNING id", ruvia::db_driver::postgresql>(name);
            if (result_value.empty() || result_value.front().empty()) {
                throw std::runtime_error("PostgreSQL INSERT did not return an id");
            }
            id = result_value.front()["id"].as<std::uint64_t>().value();
        } else {
            const auto result_value = co_await c.db().execute<"INSERT INTO users(name) VALUES (?)">(name);
            id = result_value.last_insert_id().value_or(0);
        }
        co_return;
    }

    static ruvia::task<void> transfer_funds(ruvia::context& c) {
        auto tx = co_await c.db().begin_transaction();
        (void)co_await tx.execute(driver_ == ruvia::db_driver::postgresql
                                      ? "UPDATE accounts SET balance = balance - $1 WHERE id = $2"
                                      : "UPDATE accounts SET balance = balance - ? WHERE id = ?",
            100, 1);
        (void)co_await tx.execute(driver_ == ruvia::db_driver::postgresql
                                      ? "UPDATE accounts SET balance = balance + $1 WHERE id = $2"
                                      : "UPDATE accounts SET balance = balance + ? WHERE id = ?",
            100, 2);
        co_await tx.commit();
        co_return;
    }

    static void append_unsigned(std::pmr::string& output, std::uint64_t value) {
        char buffer[32]{};
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    }

    static inline ruvia::db_driver driver_{ruvia::db_driver::mariadb};
};

int main() {
    auto& app = ruvia::app();
    app.load_dotenv();
    const example::environment env_value(&app.env());

    const auto config = db_config_from_env(env_value);
    if (!config.username_.empty() && !config.database_.empty()) {
        static const std::array mariadb_migrations{
            ruvia::db_migration{{.id_ = "001_create_users",
                .sql_ = "CREATE TABLE IF NOT EXISTS users ("
                        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                        "name VARCHAR(120) NOT NULL)"}},
            ruvia::db_migration{{.id_ = "002_create_accounts",
                .sql_ = "CREATE TABLE IF NOT EXISTS accounts ("
                        "id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
                        "balance BIGINT NOT NULL)"}},
        };
        static const std::array postgresql_migrations{
            ruvia::db_migration{{.id_ = "001_create_users",
                .sql_ = "CREATE TABLE IF NOT EXISTS users ("
                        "id BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY,"
                        "name VARCHAR(120) NOT NULL)"}},
            ruvia::db_migration{{.id_ = "002_create_accounts",
                .sql_ = "CREATE TABLE IF NOT EXISTS accounts ("
                        "id BIGINT PRIMARY KEY,"
                        "balance BIGINT NOT NULL)"}},
        };

        if (env_value.get<bool>("RUVIA_DB_MIGRATE").value_or(false)) {
            if (config.driver_ == ruvia::db_driver::postgresql) {
                (void)ruvia::db_migrator::migrate(config, postgresql_migrations);
            } else {
                (void)ruvia::db_migrator::migrate(config, mariadb_migrations);
            }
        }
        database_controller::set_driver(config.driver_);
        app.database({.config_ = config});
    }

    app.listen({.address_ = "0.0.0.0", .http_ = 8086})
        .server({.worker_count_ = 2,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
        .run();
}
