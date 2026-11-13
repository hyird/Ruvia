// Direct SQL, buffered/streamed rows, transactions, and startup migrations.
// Build with a SQL backend and set RUVIA_DB_DRIVER=postgresql or mariadb,
// RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE. Run on port 8086. Startup fails
// unless RUVIA_DB_USER and RUVIA_DB_DATABASE name a database to serve.
// RUVIA_DB_MIGRATE=true explicitly enables the demo schema migrations.
// RUVIA_DB_POOL_SIZE selects connections per worker and database alias (default 1).
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
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_request.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/db.h"

#include "backend_config.h"

namespace {

ruvia::db_config db_config_from_env(const example::environment& env_value) {
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    const auto driver = env_value.get("RUVIA_DB_DRIVER");
    const auto selected = driver && *driver == "postgresql" ? ruvia::db_driver::postgresql : ruvia::db_driver::mariadb;
#elif defined(RUVIA_ENABLE_MARIADB)
    const auto selected = ruvia::db_driver::mariadb;
#else
    const auto selected = ruvia::db_driver::postgresql;
#endif
    auto config = example::database_config(env_value, selected);
    config.acquire_timeout_ = std::chrono::seconds(2);
    config.connect_timeout_ = std::chrono::seconds(5);
    config.query_timeout_ = std::chrono::seconds(30);
    return config;
}

}  // namespace

class database_controller final {
public:
    static void set_driver(ruvia::db_driver driver) noexcept {
        driver_ = driver;
    }

    RUVIA_CONTROLLER_GROUP("/db")

    RUVIA_ROUTES_BEGIN(database_controller)
    RUVIA_GET("/users/:id", find_user);
    RUVIA_GET("/users", stream_users);
    RUVIA_POST("/users", create_user);
    RUVIA_POST("/transfer", transfer);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> find_user(ruvia::context& c) {
        bool found = false;
        co_await load_user_found(c, found);
        std::string body;
        body.append(found ? "found\n" : "not found\n");
        c.draft().status(found ? ruvia::http_status::ok : ruvia::http_status::not_found);
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> stream_users(ruvia::context& c) {
        std::string body;
        co_await append_users(c, body);
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> create_user(ruvia::context& c) {
        const auto name = co_await c.req().text();
        std::uint64_t id = 0;
        co_await insert_user(c, name, id);
        std::string body;
        body.append("created id=");
        ruvia::http_append_unsigned(body, id);
        body.push_back('\n');
        c.draft().status(ruvia::http_status::created);
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

    static ruvia::task<void> append_users(ruvia::context& c, std::string& body) {
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

    static inline ruvia::db_driver driver_{ruvia::db_driver::mariadb};
};

int main() {
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());

        const auto config = db_config_from_env(env_value);
        if (config.username_.empty() || config.database_.empty()) {
            throw std::invalid_argument("no database configured: set RUVIA_DB_USER and RUVIA_DB_DATABASE");
        }
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
                (void)example::apply_migrations(config, postgresql_migrations);
            } else {
                (void)example::apply_migrations(config, mariadb_migrations);
            }
        }
        database_controller::set_driver(config.driver_);
        app.database({.config_ = config});

        app.listen({.address_ = "0.0.0.0", .http_ = 8086})
            .server({.worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}

// Structured SQL, portable bound parameters, PostgreSQL queries, schema
// migrations, procedural blocks, and TimescaleDB policy generation.
// Build with RUVIA_ENABLE_POSTGRESQL=ON or RUVIA_ENABLE_MARIADB=ON.
// Run with no arguments: print SQL without connecting to or changing a database.
// database.cpp executes queries; orm*.cpp shows entity/repository operations.
// Review generated migrations before applying them through db_migrator at startup.

namespace sql_queries_example {

void print_query(const ruvia::db_query& query, ruvia::db_driver driver) {
    const auto statement = query.compile(driver);
    std::cout << statement.sql() << "\nparameters=" << statement.params().size() << "\n\n";
}

void portable_queries() {
    ruvia::db_query query;
    const auto total = query.aggregate("sum", {query.column("amount", "o")});
    query.select({query.column("name", "u"), query.alias(total, "total")})
        .from("users", "u")
        .join(ruvia::db_join_type::inner, "orders",
            query.binary(query.column("id", "u"), ruvia::db_binary_operator::equal, query.column("user_id", "o")), "o")
        .where(query.between(query.column("amount", "o"), query.value(10), query.value(1000)))
        .group_by({query.column("id", "u"), query.column("name", "u")})
        .having(query.binary(total, ruvia::db_binary_operator::greater, query.value(100)))
        .order_by(total, ruvia::db_order_direction::desc)
        .limit(10)
        .offset(0);
    // Values become parameters; identifier quoting and placeholders follow the
    // selected driver. Never concatenate untrusted input into sql() syntax.
    print_query(query, ruvia::db_driver::postgresql);
    print_query(query, ruvia::db_driver::mariadb);

    ruvia::db_query insert;
    insert.insert_into("users", {"id", "name"}).values({insert.value(7), insert.value("Ada")});
    print_query(insert, ruvia::db_driver::postgresql);
    ruvia::db_query update;
    update.update("users").set("name", update.value("Grace")).where(update.binary(update.column("id"), ruvia::db_binary_operator::equal, update.value(7)));
    print_query(update, ruvia::db_driver::mariadb);
    ruvia::db_query remove;
    remove.delete_from("users")
        .where(remove.binary(remove.column("id"), ruvia::db_binary_operator::equal, remove.value(7)));
    print_query(remove, ruvia::db_driver::postgresql);
}

void postgres_queries() {
    ruvia::db_query candidates;
    candidates.select(candidates.column("id")).from("jobs").where(candidates.binary(candidates.column("state"), ruvia::db_binary_operator::equal, candidates.value("ready"))).order_by(candidates.column("id")).limit(5).lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true});
    ruvia::db_query claim;
    claim.with("candidates", candidates)
        .update("jobs", "j")
        .set("state", claim.value("running"))
        .update_from("candidates", "c")
        .where(claim.binary(claim.column("id", "j"), ruvia::db_binary_operator::equal, claim.column("id", "c")))
        .returning({claim.column("id", "j")});
    print_query(claim, ruvia::db_driver::postgresql);

    ruvia::db_query ranked;
    const ruvia::db_expr partitions[]{ranked.column("state")};
    const ruvia::db_order_term window_order[]{{ranked.column("id")}};
    ranked.select({ranked.column("id"),
                      ranked.alias(ranked.over(ranked.call("row_number"),
                                       {.partition_by_ = partitions, .order_by_ = window_order}),
                          "rank")})
        .from("jobs");
    print_query(ranked, ruvia::db_driver::postgresql);
    // Expressions borrow their query. Use import_expression() when composing
    // expressions from a different query.
}

void migrations() {
    ruvia::db_query expressions;
    ruvia::db_schema schema({.driver_ = ruvia::db_driver::postgresql});
    const std::array<std::string_view, 3> quality{"raw", "checked", "invalid"};
    schema.create_enum("sample_quality", quality);
    schema.create_table({.name_ = "samples",
        .columns_ = {{.name_ = "device", .type_ = {.data_type_ = ruvia::db_data_type::text}},
            {.name_ = "value", .type_ = {.data_type_ = ruvia::db_data_type::double_value}},
            {.name_ = "quality", .type_ = {.custom_name_ = "sample_quality"}, .default_value_ = expressions.value("raw")},
            {.name_ = "recorded_at", .type_ = {.data_type_ = ruvia::db_data_type::timestamp_tz}}},
        .if_not_exists_ = true});
    schema.add_column("samples", {.name_ = "label", .type_ = {.data_type_ = ruvia::db_data_type::text}, .nullable_ = true}, true);
    schema.set_column_default("samples", "label", expressions.value("unknown"));
    schema.create_index({.name_ = "samples_device_time", .table_ = "samples", .keys_ = {{.column_ = "device"}, {.column_ = "recorded_at", .order_ = ruvia::db_order_direction::desc}}, .if_not_exists_ = true});
    for (const auto& migration : schema.compile("samples_001")) {
        std::cout << migration.id() << '\n'
                  << migration.sql() << "\n\n";
    }

    // Procedural migration blocks own their emitted SQL. Application values
    // still enter through value(); sql() below contains only trusted literals.
    ruvia::db_procedure procedure;
    procedure.declare_variable({.name_ = "revision", .type_ = {.data_type_ = ruvia::db_data_type::integer}, .default_value_ = expressions.value(1)});
    procedure.begin_if(expressions.binary(expressions.column("revision"), ruvia::db_binary_operator::less, expressions.value(2)));
    procedure.assign("revision", expressions.value(2));
    procedure.otherwise();
    procedure.raise_exception("unexpected revision");
    procedure.end_if();
    std::cout << procedure.body() << '\n';

    // These migrations additionally require the TimescaleDB server extension
    // and privileges to install it. Generation itself has no server dependency.
    ruvia::db_schema timescale({.driver_ = ruvia::db_driver::postgresql});
    timescale.create_extension("timescaledb");
    timescale.create_hypertable({.table_ = "samples", .time_column_ = "recorded_at", .chunk_interval_ = expressions.sql("INTERVAL '1 day'"), .if_not_exists_ = true});
    timescale.set_compression("samples", {.segment_by_ = {"device"}, .order_by_ = {{.column_ = "recorded_at", .direction_ = ruvia::db_order_direction::desc}}});
    timescale.add_compression_policy("samples", expressions.sql("INTERVAL '7 days'"));
    timescale.add_retention_policy("samples", expressions.sql("INTERVAL '90 days'"));
    for (const auto& migration : timescale.compile("samples_timescale_002")) {
        std::cout << migration.id() << '\n'
                  << migration.sql() << '\n';
    }
}

}  // namespace sql_queries_example

[[maybe_unused]] int run_sql_queries_main() {
    using namespace sql_queries_example;
    try {
        portable_queries();
        postgres_queries();
        migrations();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
