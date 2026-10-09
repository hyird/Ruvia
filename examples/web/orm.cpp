// PostgreSQL ORM example. With no arguments, print the generated migration. --migrate applies the demo migration; --run executes the demo on the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Covers typed CRUD, projections,
// CTE writes, locks, returning rows, arrays, trigger functions and query caches.
// RUVIA_ORM_CACHE_REDIS_HOST/PORT opt into the Redis cache provider when built.
// Use a demo database: --run inserts, changes and deletes demonstration rows.
// backend_tls.h defines RUVIA_DB_TLS/CA/CERT/KEY and equivalent cache options.

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_schema.h"

#include "backend_tls.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_client.h"
#endif

namespace {
using namespace ruvia;

RUVIA_DB_ENTITY(device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, db_column_options{.primary_key_ = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(revision, std::int64_t, db_column_options{.default_expression_ = fixed_string{"1"}}),
    RUVIA_DB_COLUMN(labels, std::pmr::vector<std::pmr::string>, db_column_options{.default_expression_ = fixed_string{"ARRAY[]::text[]"}}))

RUVIA_DB_PROJECTION(device_summary,
    RUVIA_DB_COLUMN(id, std::int64_t),
    RUVIA_DB_COLUMN(label, std::pmr::string),
    RUVIA_DB_COLUMN(rank, std::int64_t))

auto migrations() {
    db_expressions expressions;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<device>({.if_not_exists_ = true});
    schema.create_index({.name_ = "orm_demo_device_labels", .table_ = device::table_name().data(), .keys_ = {{.column_ = "labels"}}, .if_not_exists_ = true, .method_ = db_index_method::gin});

    db_procedure version;
    auto changed = expressions.binary(expressions.column("name", "new"), db_binary_operator::is_distinct_from, expressions.column("name", "old"));
    version.begin_if(changed);
    version.assign("new.revision", expressions.binary(expressions.column("revision", "old"), db_binary_operator::add, expressions.value(1)));
    version.end_if();
    version.return_value(expressions.column("new"));
    schema.create_trigger_function("orm_demo_version", version, true);
    schema.drop_trigger(device::table_name(), "orm_demo_version", true);
    schema.create_trigger({.name_ = "orm_demo_version", .table_ = std::string(device::table_name()), .function_ = "orm_demo_version", .events_ = {db_trigger_event::update}});
    return schema.compile("orm_demo_001");
}

task<void> demonstrate(db_client& db) {
    auto devices = db.get_repository<device>();
    device input;
    input.set<"id">(1);
    input.set<"name">("Pump station");
    input.set<"revision">(1);
    std::pmr::vector<std::pmr::string> labels;
    labels.emplace_back("telemetry");
    labels.emplace_back("zone-a");
    input.set<"labels">(std::move(labels));
    const db_upsert_options upsert_options{.conflict_paths_ = {"id"}, .skip_update_if_no_values_changed_ = true};
    co_await devices.upsert(input, upsert_options);

    const db_find_options find_options{.where_ = device::column<"id">().between(1, 100) && device::column<"name">().like("%station%") && device::column<"labels">().array_contains({"telemetry"}),
        .order_ = {{"id", db_order_direction::asc}},
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

    auto builder = devices.create_query_builder("d");
    builder.where(device::column<"revision">() >= 1);
    std::cout << "matching=" << co_await builder.get_count() << '\n';
    builder.take(1);
    auto [page, matching] = co_await builder.get_many_and_count();
    std::cout << "page=" << page.size() << ", matching=" << matching << '\n';

    db_expressions expressions;
    auto summary = devices.create_query_builder("d");
    summary.select({{"id"},
        {"label", expressions.call("upper", {expressions.column("name", "d")})},
        {"rank", expressions.over(expressions.call("row_number"), {.order_by_ = {{expressions.column("id", "d")}}})}});
    const auto summaries = co_await summary.get_many<device_summary>();
    for (const auto& item : summaries) {
        std::cout << item.get<"rank">() << ": " << item.get<"label">() << '\n';
    }
    const std::array changes{db_assignment{"name", expressions.call("upper", {expressions.column("name")})}};
    const std::array returning{db_selection{"id"}, db_selection{"label", expressions.column("name")}};
    const auto changed = co_await devices.update_returning<device_summary>(device::column<"id">() == 1, changes, returning);
    for (const auto& item : changed) {
        std::cout << "updated=" << item.get<"label">() << '\n';
    }

    auto transaction = co_await db.begin_transaction();
    auto transactional = transaction.get_repository<device>();
    auto candidates = transactional.create_query_builder("candidate");
    candidates.where(device::column<"id">() == 1).take(1).set_lock({.mode_ = db_row_lock::update, .skip_locked_ = true});
    auto claim = transactional.create_update_builder("target");
    claim.update_from_cte("candidates", "candidate")
        .set("name", expressions.value("Claimed device"))
        .where(expressions.binary(expressions.column("id", "target"), db_binary_operator::equal, expressions.column("id", "candidate")))
        .returning();
    auto claimed = transactional.create_query_builder("claimed");
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

task<void> demonstrate_cache(db_client& db) {
    auto devices = db.get_repository<device>();
    const db_find_options options{.order_ = {{"id"}}, .take_ = 20, .cache_ = db_cache_options{.id_ = "device-page", .milliseconds_ = std::chrono::seconds(30)}};
    auto [page, total] = co_await devices.find_and_count(options);
    std::cout << "cached page=" << page.size() << ", total=" << total << '\n';
    auto query = devices.create_query_builder();
    query.cache(std::chrono::seconds(5));
    std::cout << "cached count=" << co_await query.get_count() << '\n';
    const std::array<std::string_view, 2> ids{"device-page", "device-page-count"};
    co_await db.query_result_cache().remove(ids);
}

task<void> run(const db_config& settings, event_loop_attachment& attachment) {
    std::exception_ptr failure;
    std::unique_ptr<db_client> db;
#ifdef RUVIA_ENABLE_REDIS
    std::unique_ptr<redis_client> redis;
#endif
    try {
        if (const auto* port = std::getenv("RUVIA_ORM_CACHE_REDIS_PORT")) {
#ifdef RUVIA_ENABLE_REDIS
            const auto value = std::stoul(port);
            if (value == 0 || value > 65535) {
                throw std::invalid_argument("invalid cache Redis port");
            }
            redis_config config{.port_ = static_cast<std::uint16_t>(value)};
            config.tls_ = example::backend_tls("RUVIA_ORM_CACHE_REDIS");
            if (const auto* host = std::getenv("RUVIA_ORM_CACHE_REDIS_HOST")) {
                config.host_ = host;
            }
            redis = std::make_unique<redis_client>(attachment.loop(), config);
            co_await redis->connect();
            auto store_value = redis->with_options({});
            db = std::make_unique<db_client>(attachment.loop(), settings, store_value,
                db_cache_config{.name_space_ = "orm-demo"});
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

db_config config() {
    db_config result_value{.driver_ = db_driver::postgresql};
    result_value.tls_ = example::backend_tls("RUVIA_DB");
    auto read = [](const char* key, std::string& target) {
        if (const auto* value = std::getenv(key)) {
            target = value;
        }
    };
    read("RUVIA_DB_HOST", result_value.host_);
    read("RUVIA_DB_USER", result_value.username_);
    read("RUVIA_DB_PASSWORD", result_value.password_);
    read("RUVIA_DB_DATABASE", result_value.database_);
    if (const auto* port = std::getenv("RUVIA_DB_PORT")) {
        const auto value = std::stoul(port);
        if (value == 0 || value > 65535) {
            throw std::invalid_argument("invalid database port");
        }
        result_value.port_ = static_cast<std::uint16_t>(value);
    }
    result_value.connect_timeout_ = std::chrono::seconds(5);
    result_value.query_timeout_ = std::chrono::seconds(10);
    return result_value;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        bool migrate = false, execute = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--migrate") {
                migrate = true;
            } else if (std::string_view(argv[i]) == "--run") {
                execute = true;
            } else {
                throw std::invalid_argument("usage: ruvia_example_orm [--migrate] [--run]");
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
            (void)db_migrator::migrate(settings, changes);
        }
        if (execute) {
            asio::io_context context_value(1);
            auto attachment = attach_event_loop(context_value);
            auto root = attachment.loop().start(run(settings, attachment));
            attachment.run();
            root.get();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
