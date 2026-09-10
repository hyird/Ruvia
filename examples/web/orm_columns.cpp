// PostgreSQL computed columns and transaction options. Print the migration by
// default; --migrate --run applies it and exercises the repository against the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/db/DbSchema.h"

namespace {
using namespace ruvia;

RUVIA_DB_ENTITY(LineItem, "orm_column_line_item",
    RUVIA_DB_COLUMN(id, std::int64_t, DbColumnOptions{.primaryKey = true}),
    RUVIA_DB_COLUMN(quantity, std::int64_t),
    RUVIA_DB_COLUMN(unit_price, std::int64_t),
    RUVIA_DB_COLUMN(total, std::int64_t, DbColumnOptions{.generatedType = DbGeneratedType::kStored}))

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

auto migrations() {
    DbQuery expressions;
    DbSchema schema({.driver = DbDriver::kPostgreSql});
    schema.createTable<LineItem>({.generatedColumns = {{"total", expressions.binary(
                                                                     expressions.column("quantity"), DbBinaryOperator::kMultiply,
                                                                     expressions.column("unit_price"))}}});
    return schema.compile("orm_columns_001");
}

Task<void> demonstrate(DbClient& db, DbClient& concurrent) {
    auto items = db.getRepository<LineItem>();
    LineItem input;
    input.set<"id">(1);
    input.set<"quantity">(3);
    input.set<"unit_price">(7);
    // Even explicitly supplied computed values are excluded from writes.
    input.set<"total">(-1);
    co_await items.deleteBy(LineItem::column<"id">() == 1);
    co_await items.insert(input);
    const auto inserted = co_await items.findOne({.where = LineItem::column<"id">() == 1});
    require(inserted && inserted->get<"total">() == 21, "computed INSERT value differs");

    LineItem changes;
    changes.set<"quantity">(4);
    changes.set<"total">(-2);
    co_await items.update(LineItem::column<"id">() == 1, changes);
    const auto updated = co_await items.findOne({.where = LineItem::column<"id">() == 1});
    require(updated && updated->get<"total">() == 28, "computed UPDATE value differs");
    input.set<"quantity">(5);
    co_await items.upsert(input, {.conflictPaths = {"id"}});
    const auto retained = co_await items.findOne({.where = LineItem::column<"id">() == 1});
    require(retained && retained->get<"total">() == 35, "computed UPSERT value differs");

    auto snapshot = co_await db.beginTransaction({.isolation = DbTransactionIsolation::kRepeatableRead,
        .accessMode = DbTransactionAccessMode::kReadOnly});
    auto snapshotItems = snapshot.getRepository<LineItem>();
    const auto before = co_await snapshotItems.findOne({.where = LineItem::column<"id">() == 1});
    changes.set<"quantity">(6);
    co_await concurrent.getRepository<LineItem>().update(LineItem::column<"id">() == 1, changes);
    const auto after = co_await snapshotItems.findOne({.where = LineItem::column<"id">() == 1});
    require(before && after && before->get<"total">() == 35 && after->get<"total">() == 35,
        "repeatable-read snapshot changed after another connection committed");

    bool rejected = false;
    try {
        co_await snapshotItems.update(LineItem::column<"id">() == 1, changes);
    } catch (const DbError& error) {
        rejected = error.code() == DbError::Code::kStatementFailed;
    }
    require(rejected, "read-only transaction accepted a write");
    // Failed transactions are retired by the backend. A new transaction must
    // retain the server defaults, without inheriting read-only access.
    auto writable = co_await db.beginTransaction();
    co_await writable.getRepository<LineItem>().update(LineItem::column<"id">() == 1, changes);
    co_await writable.commit();
    const auto current = co_await items.findOne({.where = LineItem::column<"id">() == 1});
    require(current && current->get<"total">() == 42, "default transaction failed to write");
    require(retained->get<"total">() == 35, "later operation changed retained entity data");
    std::cout << "Computed INSERT/UPDATE/UPSERT, retained results, repeatable-read snapshot, "
                 "read-only rejection and default transaction verified.\n";
}

Task<void> run(DbClient& db, DbClient& concurrent, EventLoopAttachment& attachment) {
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

DbConfig config() {
    DbConfig result{.driver = DbDriver::kPostgreSql};
    const auto read = [](const char* key, std::string& target) {
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
                throw std::invalid_argument("usage: ruvia_example_orm_columns [--migrate] [--run]");
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
            (void)DbMigrator::migrate(settings, changes);
        }
        if (execute) {
            asio::io_context context(1);
            auto attachment = attachEventLoop(context);
            DbClient db(attachment.loop(), settings);
            DbClient concurrent(attachment.loop(), settings);
            auto root = attachment.loop().start(run(db, concurrent, attachment));
            attachment.run();
            root.get();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
