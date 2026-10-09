// PostgreSQL computed columns and transaction options. Print the migration by
// default; --migrate --run applies it and exercises the repository against the
// database selected by RUVIA_DB_HOST/PORT/USER/PASSWORD/DATABASE.
// Build with RUVIA_ENABLE_POSTGRESQL=ON. Includes generated columns, RETURNING,
// retained entity results, transaction isolation and read-only transactions.
// backend_tls.h defines RUVIA_DB_TLS/CA/CERT/KEY for database transport.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/web/db/db_client.h"
#include "ruvia/web/db/db_schema.h"

#include "backend_tls.h"

namespace {
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
    auto items = db.get_repository<line_item>();
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
    co_await items.upsert(input, {.conflict_paths_ = {"id"}});
    const auto retained = co_await items.find_one({.where_ = line_item::column<"id">() == 1});
    require(retained && retained->get<"total">() == 35, "computed UPSERT value differs");

    auto snapshot = co_await db.begin_transaction({.isolation_ = db_transaction_isolation::repeatable_read,
        .access_mode_ = db_transaction_access_mode::read_only});
    auto snapshot_items = snapshot.get_repository<line_item>();
    const auto before = co_await snapshot_items.find_one({.where_ = line_item::column<"id">() == 1});
    changes.set<"quantity">(6);
    co_await concurrent.get_repository<line_item>().update(line_item::column<"id">() == 1, changes);
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
    auto writable = co_await db.begin_transaction();
    co_await writable.get_repository<line_item>().update(line_item::column<"id">() == 1, changes);
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
    db_config result_value{.driver_ = db_driver::postgresql};
    result_value.tls_ = example::backend_tls("RUVIA_DB");
    const auto read = [](const char* key, std::string& target) {
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
            (void)db_migrator::migrate(settings, changes);
        }
        if (execute) {
            asio::io_context context_value(1);
            auto attachment = attach_event_loop(context_value);
            db_client db(attachment.loop(), settings);
            db_client concurrent(attachment.loop(), settings);
            auto root = attachment.loop().start(run(db, concurrent, attachment));
            attachment.run();
            root.get();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
