#include <array>
#include <memory_resource>
#include <string>

#include "ruvia/web/db/DbPredicate.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using Op = DbBinaryOperator;
using Entity = DbEntity<"device", DbColumn<"id", std::int64_t>, DbColumn<"name", std::pmr::string>>;

RUVIA_TEST(db_query_reuses_postgresql_parameters_for_grouped_expressions) {
    DbQuery query;
    const auto bucket = query.binary(query.column("id"), Op::kAdd, query.value(1));
    query.select({bucket, query.aggregate("count", {query.star()})}).from("events").groupBy({bucket});
    const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(postgres.sql(), "SELECT (\"id\" + $1), \"count\"(*) FROM \"events\" GROUP BY (\"id\" + $1)");
    RUVIA_CHECK_EQ(postgres.params().size(), std::size_t{1});
    const auto maria = query.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK_EQ(maria.params().size(), std::size_t{2});
}

RUVIA_TEST(db_query_binds_values_in_wire_order_and_preserves_zero_limit) {
    DbQuery query;
    const auto second = query.value(DbValue("O'Reilly $1 ?"));
    const auto first = query.value(DbValue(9));
    const auto id = query.column("id", "u");
    query.select(first).from("users", "u").where(query.binary(id, Op::kEqual, second));
    query.orderBy(id, DbOrderDirection::kDesc).limit(0).lock({.nowait = true});
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT $1 FROM \"users\" AS \"u\" WHERE (\"u\".\"id\" = $2) ORDER BY \"u\".\"id\" DESC LIMIT $3 FOR UPDATE NOWAIT");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
    RUVIA_CHECK_EQ(detail::DbValueAccess::signedValue(statement.params()[0]), 9);
    RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[1]), "O'Reilly $1 ?");
    RUVIA_CHECK_EQ(detail::DbValueAccess::signedValue(statement.params()[2]), 0);
}

RUVIA_TEST(db_query_nested_ctes_are_owned_and_use_one_parameter_sequence) {
    DbQuery query;
    {
        DbQuery incoming;
        const std::array row{incoming.value(DbValue(7)), incoming.value(DbValue("first"))};
        incoming.values(row);
        query.with("incoming", incoming, {.materialization = DbMaterialization::kMaterialized, .columns = {"id", "name"}});
        incoming.values(row);
    }
    DbQuery filter;
    filter.select(filter.column("id")).from("incoming").where(filter.binary(filter.column("name"), Op::kEqual, filter.value(DbValue("first"))));
    query.select(query.value(DbValue(11))).from(filter, "filtered");
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "WITH \"incoming\" (\"id\", \"name\") AS MATERIALIZED (VALUES ($1, $2)) SELECT $3 FROM (SELECT \"id\" FROM \"incoming\" WHERE (\"name\" = $4)) AS \"filtered\"");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{4});
    RUVIA_CHECK_EQ(detail::DbValueAccess::signedValue(statement.params()[2]), 11);
}

RUVIA_TEST(db_query_null_comparisons_and_empty_membership_have_defined_semantics) {
    DbQuery query;
    const auto id = query.column("id");
    const std::array projections{query.binary(id, Op::kEqual, query.nullValue()), query.binary(query.nullValue(), Op::kNotEqual, id),
        query.binary(id, Op::kIn, query.list({})), query.binary(id, Op::kNotIn, query.list({}))};
    query.select(projections).from("devices");
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT (\"id\" IS NULL), (\"id\" IS NOT NULL), FALSE, TRUE FROM \"devices\"");
    RUVIA_CHECK(statement.params().empty());
}

RUVIA_TEST(db_query_window_aggregate_filter_and_lateral_source) {
    DbQuery query;
    auto ts = query.column("ts", "e");
    auto id = query.column("id", "e");
    auto rank = query.over(query.call("row_number"), {.partitionBy = {id}, .orderBy = {{ts, DbOrderDirection::kDesc}}});
    const std::array args{id};
    const std::array order{DbOrderTerm{ts, DbOrderDirection::kAsc}};
    auto aggregate = query.filter(query.aggregate("array_agg", args, false, order), query.unary(DbUnaryOperator::kIsNotNull, id));
    const std::array projections{query.alias(rank, "rank"), aggregate};
    query.select(projections).from("events", "e");
    query.joinFunction(DbJoinType::kCross, query.call("jsonb_each", {query.column("data", "e")}), {}, "kv", {.lateral = true});
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(statement.sql().find("\"row_number\"() OVER (PARTITION BY \"e\".\"id\" ORDER BY \"e\".\"ts\" DESC)") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("\"array_agg\"(\"e\".\"id\" ORDER BY \"e\".\"ts\" ASC) FILTER (WHERE (\"e\".\"id\" IS NOT NULL))") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("CROSS JOIN LATERAL \"jsonb_each\"(\"e\".\"data\") AS \"kv\"") != std::string_view::npos);
}

RUVIA_TEST(db_query_bulk_upsert_conditions_and_returning) {
    DbQuery query;
    const std::array<std::string_view, 2> columns{"id", "revision"};
    query.insertInto("device", columns, "d");
    const std::array first{query.value(DbValue(7)), query.value(DbValue(2))};
    const std::array second{query.value(DbValue(8)), query.defaultValue()};
    query.values(first).values(second);
    const auto revision = query.column("revision", "excluded");
    query.onConflict({.columns = {"id"}, .update = {{"revision", revision}}, .updateWhere = query.binary(revision, Op::kGreater, query.column("revision", "d"))});
    const std::array returned{query.column("id")};
    query.returning(returned);
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "INSERT INTO \"device\" AS \"d\" (\"id\", \"revision\") VALUES ($1, $2), ($3, DEFAULT) ON CONFLICT (\"id\") DO UPDATE SET \"revision\" = \"excluded\".\"revision\" WHERE (\"excluded\".\"revision\" > \"d\".\"revision\") RETURNING \"id\"");
    RUVIA_CHECK(statement.returnsRows());
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
}

RUVIA_TEST(db_query_set_operations_preserve_grouping_and_nested_limits) {
    DbQuery first, second, third;
    first.select(first.value(DbValue(1)));
    second.select(second.value(DbValue(2))).limit(1);
    third.select(third.value(DbValue(3)));
    first.combine(DbSetOperation::kUnionAll, second).combine(DbSetOperation::kIntersect, third).limit(4);
    const auto statement = first.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "((SELECT $1) UNION ALL (SELECT $2 LIMIT $3)) INTERSECT (SELECT $4) LIMIT $5");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{5});
}

RUVIA_TEST(db_query_mariadb_uses_its_own_placeholders_and_null_ordering) {
    DbQuery query;
    const auto name = query.column("name");
    query.select(query.binary(name, Op::kConcat, query.value(DbValue("suffix")))).from("user");
    query.orderBy(name, DbOrderDirection::kAsc, DbNullsOrder::kLast).offset(2);
    const auto statement = query.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT CONCAT(`name`, ?) FROM `user` ORDER BY (`name` IS NULL) ASC, `name` ASC LIMIT 18446744073709551615 OFFSET ?");
    query.where(query.binary(name, Op::kILike, query.value(DbValue("needle"))));
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));
}

RUVIA_TEST(db_query_rejects_foreign_expressions_and_incompatible_clauses) {
    DbQuery first, second;
    auto foreign = first.column("id");
    RUVIA_CHECK(testing::throwsOn([&] { second.where(foreign); }));
    second.where(second.importExpression(foreign)).insertInto("device");
    RUVIA_CHECK(testing::throwsOn([&] { (void)second.compile(DbDriver::kPostgreSql, nullptr); }));
    DbQuery invalidJoin;
    invalidJoin.from("one").join(DbJoinType::kLeft, "two");
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalidJoin.compile(DbDriver::kPostgreSql, nullptr); }));
}

RUVIA_TEST(db_query_compilation_releases_temporary_allocations_and_retains_output) {
    test::CountingMemoryResource input, output;
    const auto retained = [&] {
        DbQuery query(&input);
        std::string text(500, 'x');
        query.select(query.value(DbValue(text)));
        text.assign(500, 'y');
        return query.compile(DbDriver::kPostgreSql, &output);
    }();
    RUVIA_CHECK_EQ(input.liveAllocations(), std::size_t{0});
    const auto baseline = output.liveAllocations();
    for (int i = 0; i < 20; ++i) {
        {
            DbQuery query(&input);
            query.select(query.value(DbValue("next")));
            const auto statement = query.compile(DbDriver::kPostgreSql, &output);
        }
        RUVIA_CHECK_EQ(input.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(output.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(detail::DbValueAccess::text(retained.params().front()), std::string(500, 'x'));
    RUVIA_CHECK(output.deallocationCount() > 0);
    {
        DbQuery invalid(&input);
        invalid.select(invalid.value(DbValue("allocated"))).from("devices").join(DbJoinType::kInner, "missing_on");
        RUVIA_CHECK(testing::throwsOn([&] { (void)invalid.compile(DbDriver::kPostgreSql, &output); }));
        RUVIA_CHECK_EQ(output.liveAllocations(), baseline);
    }
    RUVIA_CHECK_EQ(input.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(db_query_data_modifications_require_top_level_or_cte_placement) {
    DbQuery removed;
    removed.deleteFrom("device").returning({removed.column("id")});
    DbQuery query;
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.subquery(removed); }));
    RUVIA_CHECK(testing::throwsOn([&] { query.from(removed, "removed"); }));
    RUVIA_CHECK(testing::throwsOn([&] { query.join(DbJoinType::kCross, removed, {}, "removed"); }));
    DbQuery insert;
    insert.insertInto("archive", {"id"});
    RUVIA_CHECK(testing::throwsOn([&] { insert.insertFrom(removed); }));
    query.with("removed", removed).from("removed");
    RUVIA_CHECK_EQ(query.compile(DbDriver::kPostgreSql, nullptr).returnsRows(), true);
    DbQuery nested;
    RUVIA_CHECK(testing::throwsOn([&] { nested.from(query, "nested"); }));
    nested.with("nested", query).from("nested");
    RUVIA_CHECK(testing::throwsOn([&] { (void)nested.compile(DbDriver::kPostgreSql, nullptr); }));
}

RUVIA_TEST(db_predicate_composes_typed_fields_and_owns_literal_strings) {
    std::string name(150, 'a');
    auto condition = (Entity::column<"id">() >= 7) && (Entity::column<"name">() == name);
    name.assign(150, 'z');
    DbQuery query;
    query.from("device").where(condition.expression(query));
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT * FROM \"device\" WHERE ((\"device\".\"id\" >= $1) AND (\"device\".\"name\" = $2))");
    RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[1]), std::string(150, 'a'));
}

}  // namespace
