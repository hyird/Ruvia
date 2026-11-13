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
// See backend_config.h for RUVIA_DB_TLS/CA/CERT/KEY. Local plaintext servers
// require the explicit setting RUVIA_DB_TLS=false.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/db.h"

#include "backend_config.h"
#if defined(RUVIA_ENABLE_REDIS)
#include "ruvia/web/redis.h"
#endif
#include <tuple>
#include <utility>

#include "ruvia/web/redis.h"
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

#if defined(RUVIA_ENABLE_POSTGRESQL)
// PostgreSQL ORM example. With no arguments, print the generated migration. --migrate applies the demo migration; --run executes the demo on the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Covers typed CRUD, projections,
// CTE writes, locks, returning rows, arrays, trigger functions and query caches.
// RUVIA_ORM_CACHE_REDIS_HOST/PORT opt into the Redis cache provider when built.
// Use a demo database: --run inserts, changes and deletes demonstration rows.
// backend_config.h defines RUVIA_DB_TLS/CA/CERT/KEY and equivalent cache options.

namespace orm_base_example {
using namespace ruvia;

RUVIA_DB_ENTITY(device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::string),
    RUVIA_DB_COLUMN(revision, std::int64_t, db_column_options{.default_expression_ = fixed_string{"1"}}),
    RUVIA_DB_COLUMN(labels, std::vector<std::string>, db_column_options{.default_expression_ = fixed_string{"ARRAY[]::text[]"}}))

RUVIA_DB_PROJECTION(device_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

auto migrations() {
    db_query expressions;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<device>({.if_not_exists_ = true});
    schema.create_index({.name_ = "orm_demo_device_labels", .table_ = device::table_name(), .keys_ = {{.column_ = "labels"}}, .if_not_exists_ = true, .method_ = db_index_method::gin});

    db_procedure version;
    auto changed = expressions.binary(expressions.column("name", "new"), db_binary_operator::is_distinct_from, expressions.column("name", "old"));
    version.begin_if(changed);
    version.assign("new.revision", expressions.binary(expressions.column("revision", "old"), db_binary_operator::add, expressions.value(1)));
    version.end_if();
    version.return_value(expressions.column("new"));
    schema.create_trigger_function("orm_demo_version", version, true);
    schema.drop_trigger(device::table_name(), "orm_demo_version", true);
    schema.create_trigger({.name_ = "orm_demo_version", .table_ = device::table_name(), .function_ = "orm_demo_version", .events_ = {db_trigger_event::update}});
    return schema.compile("orm_demo_001");
}

task<void> demonstrate(db_client& db) {
    auto handle = db.db();
    auto devices = handle.repository<device>();
    device input;
    input.set<"id">(1);
    input.set<"name">("Pump station");
    input.set<"revision">(1);
    std::vector<std::string> labels;
    labels.emplace_back("telemetry");
    labels.emplace_back("zone-a");
    input.set<"labels">(std::move(labels));
    const std::array<borrowed_text, 1> upsert_conflict_paths{"id"};
    const db_upsert_options upsert_options{.conflict_paths_ = upsert_conflict_paths, .skip_update_if_no_values_changed_ = true};
    co_await devices.upsert(input, upsert_options);

    const std::array<db_find_order, 1> find_order{{{"id", db_order_direction::asc}}};
    const db_find_options find_options{.where_ = device::column<"id">().between(1, 100) && device::column<"name">().like("%station%") && device::column<"labels">().array_contains({"telemetry"}),
        .order_ = find_order,
        .take_ = 20};
    auto [found, total] = co_await devices.find_and_count(find_options);
    std::cout << "total=" << total << '\n';
    for (const auto& device : found) {
        std::cout << device.get<"id">() << ": " << device.get<"name">() << '\n';
    }

    device patch;
    patch.set<"name">("Pump station updated");
    co_await devices.update(device::column<"id">() == 1, patch);
    co_await devices.increment(device::column<"id">() == 1, "revision", 1);
    co_await devices.decrement(device::column<"id">() == 1, "revision", 1);
    std::cout << "exists=" << co_await devices.exists({.where_ = device::column<"id">() == 1}) << '\n';
    const auto one = co_await devices.find_one({.where_ = device::column<"id">() == 1});
    if (one) {
        std::cout << "revision=" << one->get<"revision">() << '\n';
    }

    auto builder = devices.query_builder("d");
    builder.where(device::column<"revision">() >= 1);
    std::cout << "matching=" << co_await builder.get_count() << '\n';
    builder.limit(1);
    auto [page, matching] = co_await builder.get_many_and_count();
    std::cout << "page=" << page.size() << ", matching=" << matching << '\n';

    db_query expressions;
    auto summary = devices.query_builder("d");
    const db_order_term summary_order[]{{expressions.column("id", "d")}};
    summary.select({{"id"},
        {"label", expressions.call("upper", {expressions.column("name", "d")})},
        {"rank", expressions.over(expressions.call("row_number"), {.order_by_ = summary_order})}});
    const auto summaries = co_await summary.get_many<device_summary>();
    for (const auto& item : summaries) {
        std::cout << item.get<"rank">() << ": " << item.get<"label">() << '\n';
    }
    auto update_summary = devices.update_builder();
    update_summary.where(device::column<"id">() == 1)
        .set({
            {"name", expressions.call("upper", {expressions.column("name")})},
            device::column<"revision">() += 1,
        })
        .returning({{"id"}, device::column<"name">().as("label"), device::column<"revision">().as("rank")});
    const auto changed = co_await update_summary.get_one<device_summary>();
    if (changed) {
        std::cout << "updated=" << changed->get<"label">() << '\n';
    }

    auto transaction = co_await handle.begin_transaction();
    auto transactional = transaction.repository<device>();
    auto candidates = transactional.query_builder("candidate");
    candidates.where(device::column<"id">() == 1).limit(1).set_lock({.mode_ = db_row_lock::update, .skip_locked_ = true});
    auto claim = transactional.update_builder("target");
    claim.update_from_cte("candidates", "candidate")
        .set("name", "Claimed device")
        .where(device::column<"id">("target") == device::column<"id">("candidate"))
        .returning();
    auto claimed = transactional.query_builder("claimed");
    claimed.with("candidates", candidates).with("claimed", claim).from_cte("claimed");
    const auto claimed_devices = co_await claimed.get_many();
    std::cout << "claimed=" << claimed_devices.size() << '\n';
    const db_find_options transaction_page{.where_ = device::column<"id">() == 1, .skip_ = 10, .take_ = 1};
    auto [empty_page, transaction_total] = co_await transactional.find_and_count(transaction_page);
    std::cout << "empty page=" << empty_page.size() << ", transaction total=" << transaction_total << '\n';
    const auto locked = co_await transactional.find_one({.where_ = device::column<"id">() == 1,
        .lock_ = db_lock_options{.mode_ = db_row_lock::update, .skip_locked_ = true}});
    if (locked) {
        co_await transactional.remove(*locked);
    }
    co_await transaction.rollback();
    std::cout << "after rollback=" << co_await devices.count() << '\n';
}

#ifdef RUVIA_ENABLE_REDIS
task<void> demonstrate_cache(db_client& db) {
    auto handle = db.db();
    auto devices = handle.repository<device>();
    const std::array<db_find_order, 1> cache_order{{{"id"}}};
    const db_find_options options{.order_ = cache_order, .take_ = 20, .cache_ = db_cache_options{.id_ = "device-page", .duration_ = std::chrono::seconds(30)}};
    auto [page, total] = co_await devices.find_and_count(options);
    std::cout << "cached page=" << page.size() << ", total=" << total << '\n';
    auto query = devices.query_builder();
    query.cache(std::chrono::seconds(5));
    std::cout << "cached count=" << co_await query.get_count() << '\n';
    // The id covers the page and the count find_and_count derived from it.
    const std::array<std::string_view, 1> ids{"device-page"};
    co_await handle.query_result_cache().remove(ids);
}
#endif

task<void> run(const db_config& settings, event_loop_attachment& attachment, const example::environment& env) {
    std::exception_ptr failure;
    std::unique_ptr<db_client> db;
#ifdef RUVIA_ENABLE_REDIS
    std::unique_ptr<redis_client> redis;
#endif
    try {
        if (const auto port = env.get<std::uint16_t>("RUVIA_ORM_CACHE_REDIS_PORT")) {
#ifdef RUVIA_ENABLE_REDIS
            auto config = example::redis_config(env, "RUVIA_ORM_CACHE_REDIS");
            redis = std::make_unique<redis_client>(attachment.loop(), config);
            co_await redis->connect();
            auto store_value = redis->redis();
            db = std::make_unique<db_client>(attachment.loop(), settings, store_value,
                db_cache_config{.key_prefix_ = "orm-demo"});
#else
            (void)port;
            throw std::invalid_argument("ORM query caching requires Redis support");
#endif
        } else {
            db = std::make_unique<db_client>(attachment.loop(), settings);
        }
        co_await db->connect();
        co_await demonstrate(*db);
#ifdef RUVIA_ENABLE_REDIS
        if (redis) {
            co_await demonstrate_cache(*db);
        }
#endif
    } catch (...) {
        failure = std::current_exception();
    }
    if (db) {
        co_await db->shutdown();
    }
#ifdef RUVIA_ENABLE_REDIS
    if (redis) {
        co_await redis->shutdown();
    }
#endif
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

db_config config(const example::environment& env) {
    return example::database_config(env, db_driver::postgresql);
}
}  // namespace orm_base_example

[[maybe_unused]] int run_orm_main(int argc, char** argv) {
    using namespace orm_base_example;
    try {
        bool migrate = false, execute = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--migrate") {
                migrate = true;
            } else if (std::string_view(argv[i]) == "--run") {
                execute = true;
            } else {
                throw std::invalid_argument("usage: ruvia_example_database [--migrate] [--run]");
            }
        }
        const auto changes = migrations();
        for (const auto& migration : changes) {
            std::cout << migration.sql() << ";\n";
        }
        if (!migrate && !execute) {
            return 0;
        }
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env(&app.env());
        const auto settings = config(env);
        if (migrate) {
            (void)example::apply_migrations(settings, changes);
        }
        if (execute) {
            asio::io_context context_value(asio::config_from_concurrency_hint(ASIO_CONCURRENCY_HINT_UNSAFE_IO));
            // The loop looks a DNS RUVIA_DB_HOST up on this pool; declared
            // first, it outlives the attachment.
            blocking_pool host_lookups({.thread_count_ = 1});
            auto attachment = attach_event_loop(context_value, {.blocking_pool_ = &host_lookups});
            auto root = attachment.loop().start(run(settings, attachment, env));
            attachment.run();
            std::move(root).get();
        }
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// PostgreSQL computed columns and transaction options. Print the migration by
// default; --migrate --run applies it and exercises the repository against the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Includes generated columns, RETURNING,
// retained entity results, transaction isolation and read-only transactions.
// backend_config.h defines RUVIA_DB_TLS/CA/CERT/KEY for database transport.

namespace orm_columns_example {
using namespace ruvia;

RUVIA_DB_ENTITY(line_item, "orm_column_line_item",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(quantity, std::int64_t),
    RUVIA_DB_COLUMN(unit_price, std::int64_t),
    RUVIA_DB_COLUMN(total, std::int64_t, db_column_options{.generated_type_ = db_generated_type::stored}))

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

auto migrations() {
    db_query expressions;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<line_item>({.generated_columns_ = {{"total", expressions.binary(
                                                                         expressions.column("quantity"), db_binary_operator::multiply,
                                                                         expressions.column("unit_price"))}}});
    return schema.compile("orm_columns_001");
}

task<void> demonstrate(db_client& db, db_client& concurrent) {
    auto session = db.db();
    auto items = session.repository<line_item>();
    line_item input;
    input.set<"id">(1);
    input.set<"quantity">(3);
    input.set<"unit_price">(7);
    // Even explicitly supplied computed values are excluded from writes.
    input.set<"total">(-1);
    co_await items.delete_by(line_item::column<"id">() == 1);
    co_await items.insert(input);
    const auto inserted = co_await items.find_one({.where_ = line_item::column<"id">() == 1});
    require(inserted && inserted->get<"total">() == 21, "computed INSERT value differs");

    line_item changes;
    changes.set<"quantity">(4);
    changes.set<"total">(-2);
    co_await items.update(line_item::column<"id">() == 1, changes);
    const auto updated = co_await items.find_one({.where_ = line_item::column<"id">() == 1});
    require(updated && updated->get<"total">() == 28, "computed UPDATE value differs");
    input.set<"quantity">(5);
    const borrowed_text conflict_paths[]{"id"};
    co_await items.upsert(input, {.conflict_paths_ = conflict_paths});
    const auto retained = co_await items.find_one({.where_ = line_item::column<"id">() == 1});
    require(retained && retained->get<"total">() == 35, "computed UPSERT value differs");

    auto snapshot = co_await session.begin_transaction({.isolation_ = db_transaction_isolation::repeatable_read,
        .access_mode_ = db_transaction_access_mode::read_only});
    auto snapshot_items = snapshot.repository<line_item>();
    const auto before = co_await snapshot_items.find_one({.where_ = line_item::column<"id">() == 1});
    changes.set<"quantity">(6);
    co_await concurrent.db().repository<line_item>().update(line_item::column<"id">() == 1, changes);
    const auto after = co_await snapshot_items.find_one({.where_ = line_item::column<"id">() == 1});
    require(before && after && before->get<"total">() == 35 && after->get<"total">() == 35,
        "repeatable-read snapshot changed after another connection committed");

    bool rejected = false;
    try {
        co_await snapshot_items.update(line_item::column<"id">() == 1, changes);
    } catch (const db_error& error) {
        rejected = error.code() == db_error::code_type::statement_failed;
    }
    require(rejected, "read-only transaction accepted a write");
    // Failed transactions are retired by the backend. A new transaction must
    // retain the server defaults, without inheriting read-only access.
    auto writable = co_await session.begin_transaction();
    co_await writable.repository<line_item>().update(line_item::column<"id">() == 1, changes);
    co_await writable.commit();
    const auto current = co_await items.find_one({.where_ = line_item::column<"id">() == 1});
    require(current && current->get<"total">() == 42, "default transaction failed to write");
    require(retained->get<"total">() == 35, "later operation changed retained entity data");
    std::cout << "Computed INSERT/UPDATE/UPSERT, retained results, repeatable-read snapshot, "
                 "read-only rejection and default transaction verified.\n";
}

task<void> run(db_client& db, db_client& concurrent, event_loop_attachment& attachment) {
    std::exception_ptr failure;
    try {
        co_await db.connect();
        co_await concurrent.connect();
        co_await demonstrate(db, concurrent);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await db.shutdown();
    co_await concurrent.shutdown();
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

db_config config() {
    auto& app = ruvia::app();
    app.load_dotenv();
    return example::database_config(example::environment(&app.env()), db_driver::postgresql);
}
}  // namespace orm_columns_example

[[maybe_unused]] int run_orm_columns_main(int argc, char** argv) {
    using namespace orm_columns_example;
    try {
        bool migrate = false, execute = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--migrate") {
                migrate = true;
            } else if (std::string_view(argv[i]) == "--run") {
                execute = true;
            } else {
                throw std::invalid_argument("usage: ruvia_example_database [--migrate] [--run]");
            }
        }
        const auto changes = migrations();
        for (const auto& migration : changes) {
            std::cout << migration.sql() << ";\n";
        }
        if (!migrate && !execute) {
            return 0;
        }
        const auto settings = config();
        if (migrate) {
            (void)example::apply_migrations(settings, changes);
        }
        if (execute) {
            asio::io_context context_value(asio::config_from_concurrency_hint(ASIO_CONCURRENCY_HINT_UNSAFE_IO));
            // The loop looks a DNS RUVIA_DB_HOST up on this pool; declared
            // first, it outlives the attachment.
            blocking_pool host_lookups({.thread_count_ = 1});
            auto attachment = attach_event_loop(context_value, {.blocking_pool_ = &host_lookups});
            db_client db(attachment.loop(), settings);
            db_client concurrent(attachment.loop(), settings);
            auto root = attachment.loop().start(run(db, concurrent, attachment));
            attachment.run();
            std::move(root).get();
        }
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// Prints the schema by default. --migrate --run creates and queries demo data
// in a PostgreSQL database selected by the RUVIA_DB_* environment variables.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Demonstrates relation loading,
// joins, junction tables, relation mutations and cascading entity operations.
// Keep writes in one explicit transaction when several entities must agree.
// backend_config.h defines RUVIA_DB_TLS/CA/CERT/KEY for database transport.

namespace ruvia::examples {

struct department;
struct employee;
struct profile;
struct user;
struct course;
struct student;

RUVIA_DB_ENTITY(department, "orm_relations.orm_department",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::string),
    RUVIA_DB_ONE_TO_MANY(employees, employee, department))

RUVIA_DB_ENTITY(employee, "orm_relations.orm_employee",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(department_id, std::int64_t),
    RUVIA_DB_COLUMN(name, std::string),
    RUVIA_DB_MANY_TO_ONE(department, department, RUVIA_DB_JOIN_COLUMN(department_id, id)))

RUVIA_DB_ENTITY(profile, "orm_relations.orm_profile",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(display_name, std::string),
    RUVIA_DB_ONE_TO_ONE(user, user, RUVIA_DB_INVERSE(profile)))

RUVIA_DB_ENTITY(user, "orm_relations.orm_user",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(profile_id, std::int64_t),
    RUVIA_DB_COLUMN(login, std::string),
    RUVIA_DB_ONE_TO_ONE(profile, profile, RUVIA_DB_JOIN_COLUMN(profile_id, id)))

using student_course_table_type = RUVIA_DB_JOIN_TABLE("orm_relations.orm_student_course",
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(student_id, id)),
    RUVIA_DB_JOIN_COLUMNS(RUVIA_DB_JOIN_COLUMN(course_id, id)));

RUVIA_DB_ENTITY(course, "orm_relations.orm_course",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(title, std::string),
    RUVIA_DB_MANY_TO_MANY(students, student, RUVIA_DB_INVERSE(courses)))

RUVIA_DB_ENTITY(student, "orm_relations.orm_student",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::string),
    RUVIA_DB_MANY_TO_MANY(courses, course, student_course_table_type))

RUVIA_DB_ENTITY(student_course, "orm_relations.orm_student_course",
    RUVIA_DB_COLUMN(student_id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(course_id, std::int64_t, db_column_options{.primary_key_ = true}))

auto migrations() {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_schema("orm_relations", true);
    // Referenced tables must be created before tables carrying foreign keys.
    schema.create_table<department>();
    schema.create_table<profile>();
    schema.create_table<course>();
    schema.create_table<employee>();
    schema.create_table<user>();
    schema.create_table<student>();
    schema.create_relation_tables<student>();
    return schema.compile("orm_relations_001");
}

task<void> query_example(db_client& db) {
    auto employees = db.db().repository<employee>();
    const std::array<borrowed_text, 2> employee_relations{"department", "department.employees"};
    const std::array<db_find_order, 1> employee_order{{{"id", db_order_direction::asc}}};
    const db_find_options employee_options{
        .relations_ = employee_relations,
        .order_ = employee_order,
        .take_ = 25,
    };
    auto [rows, total] = co_await employees.find_and_count(employee_options);
    for (const auto& employee : rows) {
        const auto& department_value = employee.get<"department">();
        (void)department_value.get<"name">();
        for (const auto& colleague : department_value.get<"employees">()) {
            (void)colleague.get<"id">();
        }
    }

    auto builder = employees.query_builder("employee");
    builder.left_join_and_select("department", "department")
        .left_join_and_select("department.employees", "colleague")
        .limit(25);
    auto [joined, joined_total] = co_await builder.get_many_and_count();
    if (rows.size() < 2 || joined.size() < 2 || total != 3 || joined_total != 3) {
        throw std::runtime_error("relation example expected at least two employees");
    }
    for (const auto& employee : joined) {
        if (employee.is_set<"department">() && !employee.is_null<"department">()) {
            (void)employee.get<"department">().get<"id">();
        }
    }
}

task<void> run(db_client& db, event_loop_attachment& attachment) {
    std::exception_ptr failure;
    try {
        co_await db.connect();
        auto departments = db.db().repository<department>();
        auto employees = db.db().repository<employee>();
        auto profiles = db.db().repository<profile>();
        auto users = db.db().repository<user>();
        auto courses = db.db().repository<course>();
        auto students = db.db().repository<student>();
        for (const auto [id, name] : {std::pair{1LL, "North"}, std::pair{2LL, "South"}}) {
            department row;
            row.set<"id">(id);
            row.set<"name">(name);
            const borrowed_text conflict_paths[]{"id"};
            co_await departments.upsert(row, {.conflict_paths_ = conflict_paths});
        }
        for (const auto [id, department_value, name] : {std::tuple{1LL, 1LL, "Alice"}, std::tuple{2LL, 1LL, "Bob"}, std::tuple{3LL, 2LL, "Carol"}}) {
            employee row;
            row.set<"id">(id);
            row.set<"department_id">(department_value);
            row.set<"name">(name);
            const borrowed_text conflict_paths[]{"id"};
            co_await employees.upsert(row, {.conflict_paths_ = conflict_paths});
        }
        profile profile;
        profile.set<"id">(1);
        profile.set<"display_name">("Demo user");
        const borrowed_text conflict_paths[]{"id"};
        co_await profiles.upsert(profile, {.conflict_paths_ = conflict_paths});
        user user;
        user.set<"id">(1);
        user.set<"profile_id">(1);
        user.set<"login">("demo");
        co_await users.upsert(user, {.conflict_paths_ = conflict_paths});
        for (const auto [id, title] : {std::pair{1LL, "Hydraulics"}, std::pair{2LL, "Controls"}}) {
            course course;
            course.set<"id">(id);
            course.set<"title">(title);
            co_await courses.upsert(course, {.conflict_paths_ = conflict_paths});
            student student;
            student.set<"id">(id);
            student.set<"name">("Learner");
            co_await students.upsert(student, {.conflict_paths_ = conflict_paths});
        }
        auto memberships = db.db().repository<student_course>();
        const borrowed_text membership_conflict_paths[]{"student_id", "course_id"};
        for (const auto [student_id, course_id] : {std::pair{1LL, 1LL}, std::pair{1LL, 2LL}, std::pair{2LL, 2LL}}) {
            student_course membership;
            membership.set<"student_id">(student_id);
            membership.set<"course_id">(course_id);
            co_await memberships.upsert(membership, {.conflict_paths_ = membership_conflict_paths, .do_nothing_ = true});
        }
        co_await query_example(db);
        const std::array<borrowed_text, 1> department_relations{"employees"};
        const std::array<db_find_order, 1> id_order{{{"id"}}};
        const db_find_options department_page_options{.relations_ = department_relations, .order_ = id_order, .take_ = 1};
        auto [department_page, department_count] = co_await departments.find_and_count(department_page_options);
        if (department_page.size() != 1 || department_count != 2 || department_page[0].get<"employees">().size() != 2) {
            throw std::runtime_error("one-to-many pagination truncated the employees");
        }
        auto department_value = co_await departments.find_one({.where_ = department::column<"id">() == 1, .relations_ = department_relations});
        if (!department_value || department_value->get<"employees">().size() != 2) {
            throw std::runtime_error("findOne did not preserve the complete collection");
        }
        auto next_page = departments.query_builder("d");
        next_page.left_join_and_select("d.employees", "e").order_by("id").offset(1).limit(1);
        auto second = co_await next_page.get_one();
        if (!second || second->get<"id">() != 2 || second->get<"employees">().size() != 1) {
            throw std::runtime_error("relation query offset did not page departments");
        }
        next_page.set_lock({.mode_ = db_row_lock::update});
        if (co_await next_page.get_count() != 2) {
            throw std::runtime_error("relation count did not count root entities");
        }
        const borrowed_text user_relations[]{"profile"};
        const borrowed_text profile_relations[]{"user"};
        auto user_rows = co_await users.find({.relations_ = user_relations});
        auto profile_rows = co_await profiles.find({.relations_ = profile_relations});
        if (user_rows.size() != 1 || profile_rows.size() != 1 || user_rows[0].get<"profile">().get<"id">() != 1 || profile_rows[0].get<"user">().get<"id">() != 1) {
            throw std::runtime_error("one-to-one owning and inverse mapping failed");
        }
        const std::array<borrowed_text, 1> course_relations{"courses.students"};
        const db_find_options course_options{.relations_ = course_relations, .order_ = id_order, .take_ = 1};
        auto [course_rows, student_count] = co_await students.find_and_count(course_options);
        if (course_rows.size() != 1 || student_count != 2 || course_rows[0].get<"courses">().size() != 2) {
            throw std::runtime_error("many-to-many pagination truncated the courses");
        }
        const borrowed_text student_relations[]{"students"};
        auto inverse_courses = co_await courses.find_one({.where_ = course::column<"id">() == 2, .relations_ = student_relations});
        if (!inverse_courses || inverse_courses->get<"students">().size() != 2) {
            throw std::runtime_error("inverse many-to-many mapping failed");
        }
        std::cout << "Loaded relation graph: employees, one-to-one profile, and course junction.\n";
    } catch (...) {
        failure = std::current_exception();
    }
    co_await db.shutdown();
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

db_config config() {
    auto& app = ruvia::app();
    app.load_dotenv();
    return example::database_config(example::environment(&app.env()), db_driver::postgresql);
}

}  // namespace ruvia::examples

[[maybe_unused]] int run_orm_relations_main(int argc, char** argv) {
    try {
        bool migrate = false, execute = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--migrate") {
                migrate = true;
            } else if (std::string_view(argv[i]) == "--run") {
                execute = true;
            } else {
                throw std::invalid_argument("usage: ruvia_example_database [--migrate] [--run]");
            }
        }
        const auto changes = ruvia::examples::migrations();
        for (const auto& migration : changes) {
            std::cout << migration.sql() << ";\n";
        }
        if (migrate) {
            (void)example::apply_migrations(ruvia::examples::config(), changes);
        }
        if (execute) {
            asio::io_context context_value(asio::config_from_concurrency_hint(ASIO_CONCURRENCY_HINT_UNSAFE_IO));
            // The loop looks a DNS RUVIA_DB_HOST up on this pool; declared
            // first, it outlives the attachment.
            ruvia::blocking_pool host_lookups({.thread_count_ = 1});
            auto attachment = ruvia::attach_event_loop(context_value, {.blocking_pool_ = &host_lookups});
            auto settings = ruvia::examples::config();
            ruvia::db_client db(attachment.loop(), settings);
            auto root = attachment.loop().start(ruvia::examples::run(db, attachment));
            attachment.run();
            std::move(root).get();
        }
        return 0;
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

#if defined(RUVIA_ENABLE_REDIS)
// Standalone PostgreSQL + Redis ORM on application-owned loops, without application.
// Read-only demo: use the orm example's orm_demo_device table (run its migration
// first). Redis HASH lookup needs ordinary Redis, not Redis Search.
// Build with RUVIA_ENABLE_POSTGRESQL=ON and RUVIA_ENABLE_REDIS=ON.
// Configure the RUVIA_DB_* and RUVIA_REDIS_HOST variables used in main().
// Each service is constructed and shut down on its own loop. Request data
// may be borrowed only while that loop and its resource owners remain alive.
// backend_config.h defines RUVIA_DB_TLS and RUVIA_REDIS_TLS plus CA/CERT/KEY.

namespace event_loop_data_example {
using namespace ruvia;

RUVIA_DB_ENTITY(device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::string))

RUVIA_REDIS_ENTITY(cached_device, "devices",
    RUVIA_REDIS_COLUMN(id, string, redis_column_options{.primary_key_ = true}),
    RUVIA_REDIS_COLUMN(name, string))

// Application composition, not a framework service registry. Each instance
// owns independent pools and memory; only its loop may use these clients.
struct worker_data final {
    worker_data(event_loop owner_value, const db_config& sql, const redis_config& cache)
        : loop_(std::move(owner_value)),
          db_(loop_, sql),
          redis_(loop_, cache) {}

    task<void> connect() {
        co_await db_.connect();
        co_await redis_.connect();
    }

    task<void> shutdown() {
        std::exception_ptr failure;
        try {
            co_await redis_.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await db_.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    task<void> read() {
        // Both operations explicitly use ORM; no raw SQL/Redis escape hatch.
        auto devices = db_.db().repository<device>();
        auto cached = redis_.redis().repository<cached_device>({.prefix_ = "ruvia:example:devices"});
        auto device = co_await devices.find_one({.where_ = device::column<"id">() == 1});
        auto cache = co_await cached.find_one({.where_ = cached_device::column<"id">() == "1"});
        // Results stay in this coroutine on the owner loop and die before
        // shutdown. Do not return client-PMR objects to the main thread.
        if (device && cache) {
            (void)(device->get<"name">() == cache->get<"name">().view());
        }
    }

    event_loop loop_;
    db_client db_;
    redis_client redis_;
};

}  // namespace event_loop_data_example

[[maybe_unused]] int run_event_loop_data_main(int argc, char** argv) {
    using namespace event_loop_data_example;
    if (argc != 2 || std::string_view(argv[1]) != "--run") {
        std::cout << "Use --run after creating orm_demo_device with the orm example.\n"
                     "Configure RUVIA_DB_HOST/USER/PASSWORD/DATABASE and RUVIA_REDIS_HOST.\n";
        return 0;
    }
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env(&app.env());
        auto sql = example::database_config(env, db_driver::postgresql);
        if (!env.get("RUVIA_DB_USER")) {
            sql.username_ = "postgres";
        }
        if (!env.get("RUVIA_DB_DATABASE")) {
            sql.database_ = "postgres";
        }
        auto cache = example::redis_config(env);
        // Both loops look DNS hosts up on this pool; IP literals need none.
        // Declared first, it outlives the loops.
        blocking_pool host_lookups({.thread_count_ = 1});
        event_loop_pool pool({.loop_count_ = 2, .blocking_pool_ = &host_lookups});
        std::vector<std::unique_ptr<worker_data>> workers;
        for (std::size_t index = 0; index < pool.loop_count(); ++index) {
            workers.push_back(std::make_unique<worker_data>(pool.loop(index), sql, cache));
        }
        pool.start();
        std::exception_ptr failure;
        std::vector<root_task<void>> tasks;
        auto observe = [&] {
            for (auto& task : tasks) {
                try {
                    std::move(task).get();
                } catch (...) {
                    if (!failure) {
                        failure = std::current_exception();
                    }
                    for (auto& worker : workers) {
                        worker->redis_.close();
                        worker->db_.close();
                    }
                }
            }
            tasks.clear();
        };
        try {
            for (auto& worker : workers) {
                tasks.push_back(worker->loop_.start(worker->connect()));
            }
            // All clients must be ready before publishing any business work.
            observe();
            if (!failure) {
                for (auto& worker : workers) {
                    tasks.push_back(worker->loop_.start(worker->read()));
                }
                observe();
            }
            // Stop business admission first; loop threads must remain running
            // until every client has cancelled and joined its operations.
            for (auto& worker : workers) {
                tasks.push_back(worker->loop_.start(worker->shutdown()));
            }
            observe();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
            // Also covers failure while launching a root task. Loop stop hooks
            // close clients; join drains all previously accepted work.
            pool.stop();
            observe();
        }
        pool.stop();
        pool.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Both workers completed SQL and Redis ORM lookups.\n";
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
#endif
#endif
