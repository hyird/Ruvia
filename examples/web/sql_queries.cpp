// Structured SQL, portable bound parameters, PostgreSQL queries, schema
// migrations, procedural blocks, and TimescaleDB policy generation.
// Build with RUVIA_ENABLE_POSTGRESQL=ON or RUVIA_ENABLE_MARIADB=ON.
// Run with no arguments: print SQL without connecting to or changing a database.
// database.cpp executes queries; orm*.cpp shows entity/repository operations.
// Review generated migrations before applying them through db_migrator at startup.

#include <array>
#include <exception>
#include <iostream>
#include <memory_resource>

#include "ruvia/web/db/db_procedure.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_schema.h"

namespace {

void print_query(const ruvia::db_query& query, ruvia::db_driver driver,
    std::pmr::memory_resource* memory) {
    const auto statement = query.compile(driver, memory);
    std::cout << statement.sql() << "\nparameters=" << statement.params().size() << "\n\n";
}

void portable_queries(std::pmr::memory_resource* memory) {
    ruvia::db_query query(memory);
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
    print_query(query, ruvia::db_driver::postgresql, memory);
    print_query(query, ruvia::db_driver::mariadb, memory);

    ruvia::db_query insert(memory);
    insert.insert_into("users", {"id", "name"}).values({insert.value(7), insert.value("Ada")});
    print_query(insert, ruvia::db_driver::postgresql, memory);
    ruvia::db_query update(memory);
    update.update("users").set("name", update.value("Grace")).where(update.binary(update.column("id"), ruvia::db_binary_operator::equal, update.value(7)));
    print_query(update, ruvia::db_driver::mariadb, memory);
    ruvia::db_query remove(memory);
    remove.delete_from("users")
        .where(remove.binary(remove.column("id"), ruvia::db_binary_operator::equal, remove.value(7)));
    print_query(remove, ruvia::db_driver::postgresql, memory);
}

void postgres_queries(std::pmr::memory_resource* memory) {
    ruvia::db_query candidates(memory);
    candidates.select(candidates.column("id")).from("jobs").where(candidates.binary(candidates.column("state"), ruvia::db_binary_operator::equal, candidates.value("ready"))).order_by(candidates.column("id")).limit(5).lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true});
    ruvia::db_query claim(memory);
    claim.with("candidates", candidates)
        .update("jobs", "j")
        .set("state", claim.value("running"))
        .update_from("candidates", "c")
        .where(claim.binary(claim.column("id", "j"), ruvia::db_binary_operator::equal, claim.column("id", "c")))
        .returning({claim.column("id", "j")});
    print_query(claim, ruvia::db_driver::postgresql, memory);

    ruvia::db_query ranked(memory);
    ranked.select({ranked.column("id"),
                      ranked.alias(ranked.over(ranked.call("row_number"),
                                       {.partition_by_ = {ranked.column("state")}, .order_by_ = {{ranked.column("id")}}}),
                          "rank")})
        .from("jobs");
    print_query(ranked, ruvia::db_driver::postgresql, memory);
    // Expressions borrow their query. Use import_expression() when composing
    // expressions from a different owner, and keep the PMR alive through results.
}

void migrations(std::pmr::memory_resource* memory) {
    ruvia::db_query expressions(memory);
    ruvia::db_schema schema({.driver_ = ruvia::db_driver::postgresql, .resource_ = memory});
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
    ruvia::db_procedure procedure(memory);
    procedure.declare_variable({.name_ = "revision", .type_ = {.data_type_ = ruvia::db_data_type::integer}, .default_value_ = expressions.value(1)});
    procedure.begin_if(expressions.binary(expressions.column("revision"), ruvia::db_binary_operator::less, expressions.value(2)));
    procedure.assign("revision", expressions.value(2));
    procedure.otherwise();
    procedure.raise_exception("unexpected revision");
    procedure.end_if();
    std::cout << procedure.body(memory) << '\n';

    // These migrations additionally require the TimescaleDB server extension
    // and privileges to install it. Generation itself has no server dependency.
    ruvia::db_schema timescale({.driver_ = ruvia::db_driver::postgresql, .resource_ = memory});
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
