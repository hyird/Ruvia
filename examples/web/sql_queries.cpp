// Structured SQL, portable bound parameters, PostgreSQL queries, schema
// migrations, procedural blocks, and TimescaleDB policy generation.
// Build with RUVIA_ENABLE_POSTGRESQL=ON or RUVIA_ENABLE_MARIADB=ON.
// Run with no arguments: print SQL without connecting to or changing a database.
// database.cpp executes queries; orm*.cpp shows entity/repository operations.
// Review generated migrations before applying them through DbMigrator at startup.

#include <array>
#include <exception>
#include <iostream>
#include <memory_resource>

#include "ruvia/web/db/DbProcedure.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/db/DbSchema.h"

namespace {

void print_query(const ruvia::DbQuery& query, ruvia::DbDriver driver,
    std::pmr::memory_resource* memory) {
    const auto statement = query.compile(driver, memory);
    std::cout << statement.sql() << "\nparameters=" << statement.params().size() << "\n\n";
}

void portable_queries(std::pmr::memory_resource* memory) {
    ruvia::DbQuery query(memory);
    const auto total = query.aggregate("sum", {query.column("amount", "o")});
    query.select({query.column("name", "u"), query.alias(total, "total")})
        .from("users", "u")
        .join(ruvia::DbJoinType::kInner, "orders",
            query.binary(query.column("id", "u"), ruvia::DbBinaryOperator::kEqual, query.column("user_id", "o")), "o")
        .where(query.between(query.column("amount", "o"), query.value(10), query.value(1000)))
        .groupBy({query.column("id", "u"), query.column("name", "u")})
        .having(query.binary(total, ruvia::DbBinaryOperator::kGreater, query.value(100)))
        .orderBy(total, ruvia::DbOrderDirection::kDesc)
        .limit(10)
        .offset(0);
    // Values become parameters; identifier quoting and placeholders follow the
    // selected driver. Never concatenate untrusted input into sql() syntax.
    print_query(query, ruvia::DbDriver::kPostgreSql, memory);
    print_query(query, ruvia::DbDriver::kMariaDb, memory);

    ruvia::DbQuery insert(memory);
    insert.insertInto("users", {"id", "name"}).values({insert.value(7), insert.value("Ada")});
    print_query(insert, ruvia::DbDriver::kPostgreSql, memory);
    ruvia::DbQuery update(memory);
    update.update("users").set("name", update.value("Grace")).where(update.binary(update.column("id"), ruvia::DbBinaryOperator::kEqual, update.value(7)));
    print_query(update, ruvia::DbDriver::kMariaDb, memory);
    ruvia::DbQuery remove(memory);
    remove.deleteFrom("users")
        .where(remove.binary(remove.column("id"), ruvia::DbBinaryOperator::kEqual, remove.value(7)));
    print_query(remove, ruvia::DbDriver::kPostgreSql, memory);
}

void postgres_queries(std::pmr::memory_resource* memory) {
    ruvia::DbQuery candidates(memory);
    candidates.select(candidates.column("id")).from("jobs").where(candidates.binary(candidates.column("state"), ruvia::DbBinaryOperator::kEqual, candidates.value("ready"))).orderBy(candidates.column("id")).limit(5).lock({.mode = ruvia::DbRowLock::kUpdate, .skipLocked = true});
    ruvia::DbQuery claim(memory);
    claim.with("candidates", candidates)
        .update("jobs", "j")
        .set("state", claim.value("running"))
        .updateFrom("candidates", "c")
        .where(claim.binary(claim.column("id", "j"), ruvia::DbBinaryOperator::kEqual, claim.column("id", "c")))
        .returning({claim.column("id", "j")});
    print_query(claim, ruvia::DbDriver::kPostgreSql, memory);

    ruvia::DbQuery ranked(memory);
    ranked.select({ranked.column("id"),
                      ranked.alias(ranked.over(ranked.call("row_number"),
                                       {.partitionBy = {ranked.column("state")}, .orderBy = {{ranked.column("id")}}}),
                          "rank")})
        .from("jobs");
    print_query(ranked, ruvia::DbDriver::kPostgreSql, memory);
    // Expressions borrow their query. Use importExpression() when composing
    // expressions from a different owner, and keep the PMR alive through results.
}

void migrations(std::pmr::memory_resource* memory) {
    ruvia::DbQuery expressions(memory);
    ruvia::DbSchema schema({.driver = ruvia::DbDriver::kPostgreSql, .resource = memory});
    const std::array<std::string_view, 3> quality{"raw", "checked", "invalid"};
    schema.createEnum("sample_quality", quality);
    schema.createTable({.name = "samples",
        .columns = {{.name = "device", .type = {.dataType = ruvia::DbDataType::kText}},
            {.name = "value", .type = {.dataType = ruvia::DbDataType::kDouble}},
            {.name = "quality", .type = {.customName = "sample_quality"}, .defaultValue = expressions.value("raw")},
            {.name = "recorded_at", .type = {.dataType = ruvia::DbDataType::kTimestampTz}}},
        .ifNotExists = true});
    schema.addColumn("samples", {.name = "label", .type = {.dataType = ruvia::DbDataType::kText}, .nullable = true}, true);
    schema.setColumnDefault("samples", "label", expressions.value("unknown"));
    schema.createIndex({.name = "samples_device_time", .table = "samples", .keys = {{.column = "device"}, {.column = "recorded_at", .order = ruvia::DbOrderDirection::kDesc}}, .ifNotExists = true});
    for (const auto& migration : schema.compile("samples_001")) {
        std::cout << migration.id() << '\n'
                  << migration.sql() << "\n\n";
    }

    // Procedural migration blocks own their emitted SQL. Application values
    // still enter through value(); sql() below contains only trusted literals.
    ruvia::DbProcedure procedure(memory);
    procedure.declareVariable({.name = "revision", .type = {.dataType = ruvia::DbDataType::kInteger}, .defaultValue = expressions.value(1)});
    procedure.beginIf(expressions.binary(expressions.column("revision"), ruvia::DbBinaryOperator::kLess, expressions.value(2)));
    procedure.assign("revision", expressions.value(2));
    procedure.otherwise();
    procedure.raiseException("unexpected revision");
    procedure.endIf();
    std::cout << procedure.body(memory) << '\n';

    // These migrations additionally require the TimescaleDB server extension
    // and privileges to install it. Generation itself has no server dependency.
    ruvia::DbSchema timescale({.driver = ruvia::DbDriver::kPostgreSql, .resource = memory});
    timescale.createExtension("timescaledb");
    timescale.createHypertable({.table = "samples", .timeColumn = "recorded_at", .chunkInterval = expressions.sql("INTERVAL '1 day'"), .ifNotExists = true});
    timescale.setCompression("samples", {.segmentBy = {"device"}, .orderBy = {{.column = "recorded_at", .direction = ruvia::DbOrderDirection::kDesc}}});
    timescale.addCompressionPolicy("samples", expressions.sql("INTERVAL '7 days'"));
    timescale.addRetentionPolicy("samples", expressions.sql("INTERVAL '90 days'"));
    for (const auto& migration : timescale.compile("samples_timescale_002")) {
        std::cout << migration.id() << '\n'
                  << migration.sql() << '\n';
    }
}

}  // namespace

int main() {
    try {
        std::pmr::unsynchronized_pool_resource memory;
        portable_queries(&memory);
        postgres_queries(&memory);
        migrations(&memory);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
