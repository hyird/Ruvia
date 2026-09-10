// PostgreSQL ORM example. With no arguments, print the generated migration and
// queries. --migrate applies the demo migration; --run executes the demo on the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/db/DbSchema.h"

namespace {
using namespace ruvia;

RUVIA_DB_ENTITY(Device, "orm_demo_device",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(name, std::pmr::string),
    RUVIA_DB_COLUMN(revision, std::int64_t),
    RUVIA_DB_COLUMN(labels, std::pmr::vector<std::pmr::string>))

auto migrations() {
    DbQuery expressions;
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTable<Device>({.defaults = {
                                    {"revision", expressions.value(1)},
                                    {"labels", expressions.cast(expressions.array({}), {.dataType = DbDataType::kText, .array = true})}},
        .ifNotExists = true});
    schema.createIndex({.name = "orm_demo_device_labels", .table = Device::tableName().data(), .keys = {{.column = "labels"}}, .ifNotExists = true, .method = DbIndexMethod::kGin});

    DbProcedure version;
    auto changed = expressions.binary(expressions.column("name", "new"), DbBinaryOperator::kIsDistinctFrom, expressions.column("name", "old"));
    version.beginIf(changed);
    version.assign("new.revision", expressions.binary(expressions.column("revision", "old"), DbBinaryOperator::kAdd, expressions.value(1)));
    version.endIf();
    version.returnValue(expressions.column("new"));
    schema.createTriggerFunction("orm_demo_version", version, true);
    schema.dropTrigger(Device::tableName(), "orm_demo_version", true);
    schema.createTrigger({.name = "orm_demo_version", .table = std::string(Device::tableName()), .function = "orm_demo_version", .events = {DbTriggerEvent::kUpdate}});
    return schema.compile("orm_demo_001");
}

DbQuery latestQuery() {
    DbQuery ranked;
    ranked.select({ranked.column("id"), ranked.column("name"), ranked.alias(ranked.over(ranked.call("row_number"), {.orderBy = {{ranked.column("revision"), DbOrderDirection::kDesc}}}), "rank")});
    ranked.from(Device::tableName());
    DbQuery query;
    query.with("ranked", ranked, {.materialization = DbMaterialization::kMaterialized});
    query.select({query.column("id"), query.column("name")}).from("ranked");
    query.where(query.binary(query.column("rank"), DbBinaryOperator::kLessEqual, query.value(10)));
    return query;
}

Task<void> demonstrate(DbClient& db) {
    auto devices = db.getRepository<Device>();
    Device input;
    input.set<"id">(1);
    input.set<"name">("Pump station");
    input.set<"revision">(1);
    std::pmr::vector<std::pmr::string> labels;
    labels.emplace_back("telemetry");
    labels.emplace_back("zone-a");
    input.set<"labels">(std::move(labels));
    const DbUpsertOptions upsertOptions{.conflictPaths = {"id"}, .skipUpdateIfNoValuesChanged = true};
    co_await devices.upsert(input, upsertOptions);

    const DbFindOptions findOptions{.where = Device::column<"id">().between(1, 100) && Device::column<"name">().like("%station%") && Device::column<"labels">().arrayContains({"telemetry"}),
        .order = {{"id", DbOrderDirection::kAsc}},
        .take = 20};
    auto [found, total] = co_await devices.findAndCount(findOptions);
    std::cout << "total=" << total << '\n';
    for (const auto& device : found) {
        std::cout << device.get<"id">() << ": " << device.get<"name">() << '\n';
    }

    Device patch;
    patch.set<"name">("Pump station updated");
    co_await devices.update(Device::column<"id">() == 1, patch);
    co_await devices.increment(Device::column<"id">() == 1, "revision", 1);
    co_await devices.decrement(Device::column<"id">() == 1, "revision", 1);
    std::cout << "exists=" << co_await devices.exists({.where = Device::column<"id">() == 1}) << '\n';
    const auto one = co_await devices.findOne({.where = Device::column<"id">() == 1});
    if (one) {
        std::cout << "revision=" << one->get<"revision">() << '\n';
    }

    auto builder = devices.createQueryBuilder("d");
    auto& query = builder.statement();
    builder.where(query.binary(builder.column<"revision">(), DbBinaryOperator::kGreaterEqual, query.value(1)));
    std::cout << "matching=" << co_await builder.getCount() << '\n';
    builder.take(1);
    auto [page, matching] = co_await builder.getManyAndCount();
    std::cout << "page=" << page.size() << ", matching=" << matching << '\n';

    auto transaction = co_await db.beginTransaction();
    auto transactional = transaction.getRepository<Device>();
    const DbFindOptions transactionPage{.where = Device::column<"id">() == 1, .skip = 10, .take = 1};
    auto [emptyPage, transactionTotal] = co_await transactional.findAndCount(transactionPage);
    std::cout << "empty page=" << emptyPage.size() << ", transaction total=" << transactionTotal << '\n';
    const auto locked = co_await transactional.findOne({.where = Device::column<"id">() == 1,
        .lock = DbLockOptions{.mode = DbRowLock::kUpdate, .skipLocked = true}});
    if (locked) {
        co_await transactional.remove(*locked);
    }
    co_await transaction.rollback();
    std::cout << "after rollback=" << co_await devices.count() << '\n';

    const auto latest = co_await db.query(latestQuery());
    std::cout << "ranked rows=" << latest.size() << '\n';
}

Task<void> run(DbClient& db, EventLoopAttachment& attachment) {
    std::exception_ptr failure;
    try {
        co_await db.connect();
        co_await demonstrate(db);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await db.shutdown();
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

DbConfig config() {
    DbConfig result{.driver = DbDriver::kPostgreSql};
    auto read = [](const char* key, std::string& target) {
        if (const auto* value = std::getenv(key)) {
            target = value;
        }
    };
    read("RUVIA_DB_HOST", result.host);
    read("RUVIA_DB_USER", result.username);
    read("RUVIA_DB_PASSWORD", result.password);
    read("RUVIA_DB_DATABASE", result.database);
    if (const auto* port = std::getenv("RUVIA_DB_PORT")) {
        const auto value = std::stoul(port);
        if (value == 0 || value > 65535) {
            throw std::invalid_argument("invalid database port");
        }
        result.port = static_cast<std::uint16_t>(value);
    }
    result.connectTimeout = std::chrono::seconds(5);
    result.queryTimeout = std::chrono::seconds(10);
    return result;
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
        const auto query = latestQuery().compile(DbDriver::kPostgreSql, nullptr);
        std::cout << query.sql() << '\n';
        if (!migrate && !execute) {
            return 0;
        }
        const auto settings = config();
        if (migrate) {
            (void)DbMigrator::migrate(settings, changes);
        }
        if (execute) {
            asio::io_context context(1);
            auto attachment = attachEventLoop(context);
            DbClient db(attachment.loop(), settings);
            auto root = attachment.loop().start(run(db, attachment));
            attachment.run();
            root.get();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
