#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/DbPredicate.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using Op = DbBinaryOperator;
using Entity = DbEntity<"device", DbColumn<"id", std::int64_t>, DbColumn<"name", std::pmr::string>>;

template <typename Fn>
std::string compileSql(Fn&& fn) {
    auto statement = fn();
    return std::string(statement.sql());
}

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

RUVIA_TEST(db_query_expression_helpers_render_postgresql_and_reject_empty_inputs) {
    DbQuery query;
    const auto left = query.column("left");
    const auto right = query.column("right");
    const auto values = query.array({query.value(1), query.value(2)});
    const std::array<DbOrderTerm, 1> orderedBy{{query.column("score"), DbOrderDirection::kDesc}};
    const auto ordered = query.withinGroup(
        query.aggregate("percentile_cont", {query.value(0.5)}),
        orderedBy);
    const auto branches = std::array{
        DbCaseBranch{query.binary(left, Op::kGreater, query.value(0)), query.value("positive")}};
    const std::array<DbQuery::Expr, 12> projections{
        query.coalesce({query.nullValue(), left}),
        query.nullIf(left, right),
        query.greatest({left, right}),
        query.least({left, right}),
        query.tuple({left, right}),
        query.any(values),
        query.all(values),
        query.caseWhen(branches, query.value("zero")),
        ordered,
        query.extract(DbDatePart::kYear, query.column("created_at")),
        query.subscript(query.column("tags"), query.value(1)),
        query.collate(query.column("name"), "C"),
    };
    query.select(projections).from("metrics");

    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(),
        "SELECT COALESCE(NULL, \"left\"), NULLIF(\"left\", \"right\"), GREATEST(\"left\", \"right\"), LEAST(\"left\", \"right\"), ROW(\"left\", \"right\"), ANY(ARRAY[$1, $2]), ALL(ARRAY[$1, $2]), CASE WHEN (\"left\" > $3) THEN $4 ELSE $5 END, \"percentile_cont\"($6) WITHIN GROUP (ORDER BY \"score\" DESC), EXTRACT(YEAR FROM \"created_at\"), (\"tags\")[$7], (\"name\" COLLATE \"C\") FROM \"metrics\"");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{7});

    DbQuery mariaArray;
    const auto array = mariaArray.array({mariaArray.value(1)});
    mariaArray.select(mariaArray.any(array)).from("metrics");
    RUVIA_CHECK(testing::throwsOn([&] { (void)mariaArray.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery mariaWithinGroup;
    const auto aggregate = mariaWithinGroup.aggregate("percentile_cont", {mariaWithinGroup.value(0.5)});
    const std::array<DbOrderTerm, 1> mariaOrder{{mariaWithinGroup.column("score"), DbOrderDirection::kAsc}};
    mariaWithinGroup.select(mariaWithinGroup.withinGroup(aggregate, mariaOrder))
        .from("metrics");
    RUVIA_CHECK(testing::throwsOn([&] { (void)mariaWithinGroup.compile(DbDriver::kMariaDb, nullptr); }));

    RUVIA_CHECK(testing::throwsOn([&] { (void)query.coalesce({}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.greatest({}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.least({}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.withinGroup(left, {}); }));
    DbQuery emptyTuple;
    emptyTuple.select(emptyTuple.tuple({})).from("metrics");
    RUVIA_CHECK(testing::throwsOn([&] { (void)emptyTuple.compile(DbDriver::kPostgreSql, nullptr); }));
}

RUVIA_TEST(db_query_native_functions_and_identifier_quoting_are_dialect_safe) {
    DbQuery query;
    const auto amount = query.column("amount", "s");
    const auto name = query.column("name", "s");
    const auto count = query.aggregate("count", {query.star()});
    const auto sum = query.aggregate("sum", {amount});
    const auto rowNumber = query.over(query.call("row_number"),
        {.orderBy = {{query.column("id", "s"), DbOrderDirection::kAsc}}});
    const auto scalar = query.call("lower", {name});
    query.select({count, sum, rowNumber, scalar}).from("sales", "s");

    const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(postgres.sql(),
        "SELECT \"count\"(*), \"sum\"(\"s\".\"amount\"), \"row_number\"() OVER (ORDER BY \"s\".\"id\" ASC), \"lower\"(\"s\".\"name\") FROM \"sales\" AS \"s\"");

    const auto maria = query.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK(maria.sql().find("count(*)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("sum(`s`.`amount`)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("row_number() OVER (ORDER BY `s`.`id` ASC)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("lower(`s`.`name`)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("`count`") == std::string_view::npos);

    DbQuery identifiers;
    const auto dangerousColumn = identifiers.column("name\"; DROP TABLE users --", "tenant.schema");
    const auto dangerousFunction = identifiers.call("pkg.weird\"function", {dangerousColumn});
    const auto specialFunction = identifiers.call("weird`fn--", {dangerousColumn});
    identifiers.select({dangerousFunction, specialFunction}).from("tenant.schema", "s");
    const auto safePostgres = identifiers.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(safePostgres.sql().find("\"tenant\".\"schema\".\"name\"\"; DROP TABLE users --\"") != std::string_view::npos);
    RUVIA_CHECK(safePostgres.sql().find("\"pkg\".\"weird\"\"function\"") != std::string_view::npos);
    const auto safeMaria = identifiers.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK(safeMaria.sql().find("`tenant`.`schema`.`name\"; DROP TABLE users --`") != std::string_view::npos);
    RUVIA_CHECK(safeMaria.sql().find("`pkg`.`weird\"function`") != std::string_view::npos);
    RUVIA_CHECK(safeMaria.sql().find("`weird``fn--`") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)identifiers.column(std::string_view("bad\0name", 8)); }));

    const std::string overlong(65, 'x');
    DbQuery overlongQuery;
    overlongQuery.select(overlongQuery.call(overlong)).from("events");
    RUVIA_CHECK(testing::throwsOn([&] { (void)overlongQuery.compile(DbDriver::kPostgreSql, nullptr); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)overlongQuery.compile(DbDriver::kMariaDb, nullptr); }));
}

RUVIA_TEST(db_query_dml_sources_distinct_and_query_joins_compile) {
    DbQuery source;
    source.select({source.column("id", "s"), source.column("name", "s")}).from("source", "s");

    DbQuery insert;
    insert.insertInto("archive", {"id", "name"}).insertFrom(source).returning({insert.column("id")});
    const auto inserted = insert.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(inserted.sql(),
        "INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\" RETURNING \"id\"");

    DbQuery update;
    update.update("target", "t").set("name", update.value("changed"));
    update.updateFrom("source", "s").where(update.binary(update.column("id", "t"), Op::kEqual, update.column("id", "s")));
    const auto updated = update.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(updated.sql(),
        "UPDATE \"target\" AS \"t\" SET \"name\" = $1 FROM \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    DbQuery removed;
    removed.deleteFrom("target", "t").deleteUsing("source", "s");
    removed.where(removed.binary(removed.column("id", "t"), Op::kEqual, removed.column("id", "s")));
    const auto deleted = removed.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(deleted.sql(),
        "DELETE FROM \"target\" AS \"t\" USING \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    DbQuery distinct;
    distinct.select({distinct.column("account_id"), distinct.column("id")})
        .distinctOn({distinct.column("account_id")})
        .from("events");
    const auto distinctStatement = distinct.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(distinctStatement.sql(),
        "SELECT DISTINCT ON (\"account_id\") \"account_id\", \"id\" FROM \"events\"");
    RUVIA_CHECK(testing::throwsOn([&] { (void)distinct.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery joined;
    joined.select(joined.star("base")).from("base", "base");
    const auto joinOn = joined.binary(joined.column("id", "base"), Op::kEqual, joined.column("id", "derived"));
    joined.join(DbJoinType::kLeft, source, joinOn, "derived");
    const std::array<std::string_view, 1> usingColumns{"id"};
    joined.join(DbJoinType::kInner, "same_ids", {}, "same_ids", usingColumns);
    const auto joinedStatement = joined.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(joinedStatement.sql(),
        "SELECT \"base\".* FROM \"base\" AS \"base\" LEFT JOIN (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\") AS \"derived\" ON (\"base\".\"id\" = \"derived\".\"id\") INNER JOIN \"same_ids\" AS \"same_ids\" USING (\"id\")");

    DbQuery functionSource;
    const auto function = functionSource.call("jsonb_each", {functionSource.column("payload")});
    functionSource.select(functionSource.star("kv"))
        .fromFunction(function, "kv",
            {.lateral = true, .withOrdinality = true, .columns = {{.name = "key"}, {.name = "value"}}});
    const auto functionStatement = functionSource.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(functionStatement.sql().find(
                    "FROM LATERAL \"jsonb_each\"(\"payload\") WITH ORDINALITY AS \"kv\" (\"key\", \"value\")") != std::string_view::npos);
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

RUVIA_TEST(db_predicate_between_and_array_operators_bind_owned_typed_values) {
    using Tagged = DbEntity<"tagged", DbColumn<"id", std::int64_t>,
        DbColumn<"tags", std::pmr::vector<std::pmr::string>>>;
    std::array<std::string, 2> input{std::string(200, 'a'), "zone-b"};
    auto condition = Tagged::column<"id">().between(3, 8) && Tagged::column<"tags">().arrayContains(std::span<const std::string>(input));
    input[0].assign(200, 'z');
    DbQuery query;
    query.from("tagged").where(condition.expression(query));
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT * FROM \"tagged\" WHERE ((\"tagged\".\"id\" BETWEEN $1 AND $2) AND (\"tagged\".\"tags\" @> CAST(ARRAY[$3, $4] AS TEXT[])))");
    RUVIA_CHECK_EQ(detail::DbValueAccess::text(statement.params()[2]), std::string(200, 'a'));
    RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery contained;
    auto subset = Tagged::column<"tags">().arrayContainedBy<std::string_view>({});
    contained.from("tagged").where(subset.expression(contained));
    const auto empty = contained.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(empty.sql(), "SELECT * FROM \"tagged\" WHERE (\"tagged\".\"tags\" <@ CAST(ARRAY[] AS TEXT[]))");
    RUVIA_CHECK(empty.params().empty());

    using Uuids = DbEntity<"uuid_tags", DbColumn<"tags", std::pmr::vector<std::pmr::string>, DbColumnOptions{.dataType = DbDataType::kUuid}>>;
    DbQuery overlap;
    auto any = Uuids::column<"tags">().arrayOverlap({"12345678-1234-1234-1234-123456789abc"});
    overlap.from("uuid_tags").where(any.expression(overlap));
    const auto typed = overlap.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK_EQ(typed.sql(), "SELECT * FROM \"uuid_tags\" WHERE (\"uuid_tags\".\"tags\" && CAST(ARRAY[$1] AS UUID[]))");
    using Codes = DbEntity<"codes", DbColumn<"values", std::pmr::vector<std::pmr::string>, DbColumnOptions{.dataType = DbDataType::kVarchar, .length = 32}>>;
    DbQuery codes;
    auto code = Codes::column<"values">().arrayContains({"zone-a"});
    codes.from("codes").where(code.expression(codes));
    const auto sized = codes.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(sized.sql().find("AS VARCHAR(32)[]") != std::string_view::npos);
}

RUVIA_TEST(db_query_binary_operator_families_cover_postgresql_and_mariadb_dialects) {
    struct BinaryCase final {
        Op op;
        std::string_view postgresToken;
        std::string_view mariaToken;
        bool listRight{false};
    };
    const std::array common{
        BinaryCase{Op::kEqual, " = ", " = "},
        BinaryCase{Op::kNotEqual, " <> ", " <> "},
        BinaryCase{Op::kLess, " < ", " < "},
        BinaryCase{Op::kLessEqual, " <= ", " <= "},
        BinaryCase{Op::kGreater, " > ", " > "},
        BinaryCase{Op::kGreaterEqual, " >= ", " >= "},
        BinaryCase{Op::kAnd, " AND ", " AND "},
        BinaryCase{Op::kOr, " OR ", " OR "},
        BinaryCase{Op::kAdd, " + ", " + "},
        BinaryCase{Op::kSubtract, " - ", " - "},
        BinaryCase{Op::kMultiply, " * ", " * "},
        BinaryCase{Op::kDivide, " / ", " / "},
        BinaryCase{Op::kModulo, " % ", " % "},
        BinaryCase{Op::kConcat, " || ", "CONCAT("},
        BinaryCase{Op::kLike, " LIKE ", " LIKE "},
        BinaryCase{Op::kNotLike, " NOT LIKE ", " NOT LIKE "},
        BinaryCase{Op::kIn, " IN ", " IN ", true},
        BinaryCase{Op::kNotIn, " NOT IN ", " NOT IN ", true},
        BinaryCase{Op::kIsDistinctFrom, " IS DISTINCT FROM ", "<=>"},
        BinaryCase{Op::kIsNotDistinctFrom, " IS NOT DISTINCT FROM ", "<=>"},
        BinaryCase{Op::kBitAnd, " & ", " & "},
        BinaryCase{Op::kBitOr, " | ", " | "},
        BinaryCase{Op::kBitXor, " # ", " ^ "},
    };
    for (const auto& item : common) {
        DbQuery query;
        const auto lhs = query.column("lhs");
        const auto rhs = item.listRight ? query.list({query.value(1), query.value(2)}) : query.column("rhs");
        query.select(query.binary(lhs, item.op, rhs)).from("cq_operator_matrix");
        const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.postgresToken) != std::string_view::npos);
        const auto maria = query.compile(DbDriver::kMariaDb, nullptr);
        RUVIA_CHECK(maria.sql().find(item.mariaToken) != std::string_view::npos);
    }

    struct PostgresOnlyCase final {
        Op op;
        std::string_view token;
    };
    const std::array postgresOnly{
        PostgresOnlyCase{Op::kILike, " ILIKE "},
        PostgresOnlyCase{Op::kNotILike, " NOT ILIKE "},
        PostgresOnlyCase{Op::kJsonGet, " -> "},
        PostgresOnlyCase{Op::kJsonGetText, " ->> "},
        PostgresOnlyCase{Op::kJsonPath, " #> "},
        PostgresOnlyCase{Op::kJsonPathText, " #>> "},
        PostgresOnlyCase{Op::kJsonContains, " @> "},
        PostgresOnlyCase{Op::kJsonContainedBy, " <@ "},
        PostgresOnlyCase{Op::kJsonHasKey, " ? "},
        PostgresOnlyCase{Op::kJsonHasAnyKey, " ?| "},
        PostgresOnlyCase{Op::kJsonHasAllKeys, " ?& "},
        PostgresOnlyCase{Op::kJsonConcat, " || "},
        PostgresOnlyCase{Op::kJsonDelete, " - "},
        PostgresOnlyCase{Op::kJsonDeletePath, " #- "},
        PostgresOnlyCase{Op::kArrayContains, " @> "},
        PostgresOnlyCase{Op::kArrayContainedBy, " <@ "},
        PostgresOnlyCase{Op::kArrayOverlap, " && "},
        PostgresOnlyCase{Op::kRegex, " ~ "},
        PostgresOnlyCase{Op::kRegexInsensitive, " ~* "},
        PostgresOnlyCase{Op::kInetContains, " >> "},
        PostgresOnlyCase{Op::kInetContainsOrEqual, " >>= "},
        PostgresOnlyCase{Op::kInetContainedBy, " << "},
        PostgresOnlyCase{Op::kInetContainedByOrEqual, " <<= "},
        PostgresOnlyCase{Op::kInetOverlap, " && "},
    };
    for (const auto& item : postgresOnly) {
        DbQuery query;
        query.select(query.binary(query.column("lhs"), item.op, query.column("rhs"))).from("cq_operator_matrix");
        const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.token) != std::string_view::npos);
        RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));
    }

    DbQuery nulls;
    const auto id = nulls.column("id");
    nulls.select({nulls.binary(id, Op::kEqual, nulls.nullValue()),
                     nulls.binary(nulls.nullValue(), Op::kNotEqual, id),
                     nulls.binary(id, Op::kIn, nulls.list({})),
                     nulls.binary(id, Op::kNotIn, nulls.list({}))})
        .from("cq_operator_matrix");
    const auto nullStatement = nulls.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(nullStatement.sql().find(" IS NULL") != std::string_view::npos);
    RUVIA_CHECK(nullStatement.sql().find(" IS NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(nullStatement.sql().find("FALSE") != std::string_view::npos);
    RUVIA_CHECK(nullStatement.sql().find("TRUE") != std::string_view::npos);

    DbQuery invalidIn;
    invalidIn.select(invalidIn.binary(invalidIn.column("id"), Op::kIn, invalidIn.value(1))).from("cq_operator_matrix");
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalidIn.compile(DbDriver::kPostgreSql, nullptr); }));
}

RUVIA_TEST(db_query_unary_operators_and_parameter_modes_cover_all_semantics) {
    struct UnaryCase final {
        DbUnaryOperator op;
        std::string_view token;
    };
    const std::array cases{
        UnaryCase{DbUnaryOperator::kNot, "NOT"},
        UnaryCase{DbUnaryOperator::kNegate, "-"},
        UnaryCase{DbUnaryOperator::kBitNot, "~"},
        UnaryCase{DbUnaryOperator::kIsNull, "IS NULL"},
        UnaryCase{DbUnaryOperator::kIsNotNull, "IS NOT NULL"},
        UnaryCase{DbUnaryOperator::kIsTrue, "IS TRUE"},
        UnaryCase{DbUnaryOperator::kIsFalse, "IS FALSE"},
        UnaryCase{DbUnaryOperator::kIsNotTrue, "IS NOT TRUE"},
        UnaryCase{DbUnaryOperator::kIsNotFalse, "IS NOT FALSE"},
    };
    for (const auto& item : cases) {
        DbQuery query;
        query.select(query.unary(item.op, query.column("value"))).from("cq_unary_matrix");
        const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.token) != std::string_view::npos);
        const auto maria = query.compile(DbDriver::kMariaDb, nullptr);
        RUVIA_CHECK(maria.sql().find(item.token) != std::string_view::npos);
    }

    DbQuery literal;
    const auto literalValue = literal.value(DbValue("O'Reilly"));
    literal.select(literalValue).from("cq_unary_matrix");
    const auto bound = literal.compile(DbDriver::kPostgreSql, nullptr, DbParameterMode::kBound);
    RUVIA_CHECK_EQ(bound.params().size(), std::size_t{1});
    const auto rendered = literal.compile(DbDriver::kPostgreSql, nullptr, DbParameterMode::kLiteral);
    RUVIA_CHECK_EQ(rendered.params().size(), std::size_t{0});
    RUVIA_CHECK(rendered.sql().find("E'O''Reilly'") != std::string_view::npos);
    const auto mariaRendered = literal.compile(DbDriver::kMariaDb, nullptr, DbParameterMode::kLiteral);
    RUVIA_CHECK(mariaRendered.sql().find("CONVERT(X'4f275265696c6c79' USING utf8mb4)") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)DbQuery::renderExpression(literalValue, DbDriver::kPostgreSql, nullptr, DbParameterMode::kBound); }));
    RUVIA_CHECK_EQ(DbQuery::renderExpression(literalValue, DbDriver::kPostgreSql, nullptr), "E'O''Reilly'");

    DbQuery invalid;
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalid.unary(DbUnaryOperator::kNot, {}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalid.binary({}, Op::kEqual, invalid.value(1)); }));
}

RUVIA_TEST(db_predicate_public_overloads_cover_comparisons_membership_nulls_and_aliases) {
    using Tagged = DbEntity<"cq_device", DbColumn<"id", std::int64_t>, DbColumn<"name", std::pmr::string>,
        DbColumn<"tags", std::pmr::vector<std::pmr::string>>>;
    RUVIA_CHECK_EQ(Tagged::column<"id">().name(), std::string_view("id"));

    const auto compile = [](DbPredicate predicate, DbDriver driver) {
        DbQuery query;
        query.from("cq_device", "d").where(predicate.expression(query, "cq_device", "d"));
        return query.compile(driver, nullptr);
    };
    const auto checkComparison = [&](DbPredicate predicate, std::string_view token) {
        const auto statement = compile(std::move(predicate), DbDriver::kPostgreSql);
        RUVIA_CHECK(statement.sql().find(token) != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("\"d\".\"id\"") != std::string_view::npos);
    };
    checkComparison(Tagged::column<"id">() == 1, " = ");
    checkComparison(Tagged::column<"id">() != 1, " <> ");
    checkComparison(Tagged::column<"id">() < 1, " < ");
    checkComparison(Tagged::column<"id">() <= 1, " <= ");
    checkComparison(Tagged::column<"id">() > 1, " > ");
    checkComparison(Tagged::column<"id">() >= 1, " >= ");

    const auto like = compile(Tagged::column<"name">().like("A%"), DbDriver::kPostgreSql);
    RUVIA_CHECK(like.sql().find(" LIKE ") != std::string_view::npos);
    const auto ilike = compile(Tagged::column<"name">().ilike("a%"), DbDriver::kPostgreSql);
    RUVIA_CHECK(ilike.sql().find(" ILIKE ") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)compile(Tagged::column<"name">().ilike("a%"), DbDriver::kMariaDb); }));
    RUVIA_CHECK(compileSql([&] { return compile(Tagged::column<"id">().isNull(), DbDriver::kPostgreSql); }).find(" IS NULL") != std::string_view::npos);
    RUVIA_CHECK(compileSql([&] { return compile(Tagged::column<"id">().isNotNull(), DbDriver::kPostgreSql); }).find(" IS NOT NULL") != std::string_view::npos);

    const std::array ids{1, 2, 3};
    const auto inSpan = compile(Tagged::column<"id">().in(std::span<const int>(ids)), DbDriver::kPostgreSql);
    RUVIA_CHECK(inSpan.sql().find(" IN ($1, $2, $3)") != std::string_view::npos);
    const auto inList = compile(Tagged::column<"id">().in({4, 5}), DbDriver::kPostgreSql);
    RUVIA_CHECK(inList.sql().find(" IN ($1, $2)") != std::string_view::npos);
    const std::span<const int> noIds;
    const auto emptyIn = compile(Tagged::column<"id">().in(noIds), DbDriver::kPostgreSql);
    RUVIA_CHECK(emptyIn.sql().find("FALSE") != std::string_view::npos);

    const auto between = compile(Tagged::column<"id">().between(2, 8), DbDriver::kPostgreSql);
    RUVIA_CHECK(between.sql().find(" BETWEEN ") != std::string_view::npos);
    const std::array<std::string_view, 2> tagValues{"one", "two"};
    const auto contains = compile(Tagged::column<"tags">().arrayContains(std::span<const std::string_view>(tagValues)), DbDriver::kPostgreSql);
    RUVIA_CHECK(contains.sql().find(" @> CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto contained = compile(Tagged::column<"tags">().arrayContainedBy({"one", "two"}), DbDriver::kPostgreSql);
    RUVIA_CHECK(contained.sql().find(" <@ CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto overlap = compile(Tagged::column<"tags">().arrayOverlap({"two"}), DbDriver::kPostgreSql);
    RUVIA_CHECK(overlap.sql().find(" && CAST(ARRAY[$1]") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)compile(Tagged::column<"tags">().arrayContains({"one"}), DbDriver::kMariaDb); }));

    auto combined = (Tagged::column<"id">() == 1) || !(Tagged::column<"name">().like("%x%"));
    const auto combinedStatement = compile(std::move(combined), DbDriver::kPostgreSql);
    RUVIA_CHECK(combinedStatement.sql().find(" OR ") != std::string_view::npos);
    RUVIA_CHECK(combinedStatement.sql().find("NOT") != std::string_view::npos);

    DbPredicate empty;
    DbQuery query;
    RUVIA_CHECK(empty.empty());
    RUVIA_CHECK(empty.expression(query).empty());
    DbQuery ownedSource;
    DbPredicate owned(ownedSource.binary(ownedSource.column("id"), Op::kEqual, ownedSource.value(1)));
    RUVIA_CHECK(!owned.empty());
    DbPredicate moved = std::move(owned);
    DbPredicate reassigned;
    reassigned = std::move(moved);
    RUVIA_CHECK(!reassigned.empty());
    DbPredicate emptyAnd;
    DbPredicate emptyOr;
    RUVIA_CHECK(testing::throwsOn([&] { (void)(std::move(emptyAnd) && (Tagged::column<"id">() == 1)); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)(std::move(emptyOr) || (Tagged::column<"id">() == 1)); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)!std::move(empty); }));
}

RUVIA_TEST(db_query_clause_source_and_join_methods_cover_state_and_dialect_rules) {
    DbQuery query;
    const auto id = query.column("id", "e");
    const auto kind = query.column("kind", "e");
    query.select(query.star("e"))
        .addSelect(query.alias(query.column("id", "e"), "selected_id"))
        .from("cq_events", "e")
        .where(query.binary(id, Op::kGreater, query.value(0)))
        .andWhere(query.binary(kind, Op::kNotEqual, query.value("archived")))
        .orWhere(query.binary(id, Op::kEqual, query.value(99)))
        .groupBy({id})
        .addGroupBy(kind)
        .having(query.binary(query.aggregate("count", {query.star()}), Op::kGreater, query.value(0)))
        .andHaving(query.binary(kind, Op::kLike, query.value("%live%")))
        .orderBy(id, DbOrderDirection::kDesc, DbNullsOrder::kFirst)
        .addOrderBy(kind, DbOrderDirection::kAsc, DbNullsOrder::kLast)
        .limit(10)
        .offset(2)
        .distinct();
    RUVIA_CHECK(query.hasWhere());
    RUVIA_CHECK(query.hasGrouping());
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(statement.sql().find("SELECT DISTINCT ") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("GROUP BY \"e\".\"id\", \"e\".\"kind\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("HAVING") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("NULLS FIRST") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("NULLS LAST") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("LIMIT") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("OFFSET") != std::string_view::npos);

    query.clearOrder().limit(std::nullopt).offset(std::nullopt).distinct(false);
    const auto cleared = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(cleared.sql().find(" ORDER BY ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find(" LIMIT ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find(" OFFSET ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find("DISTINCT") == std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { query.limit(static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1); }));
    RUVIA_CHECK(testing::throwsOn([&] { query.offset(static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1); }));

    DbQuery derived;
    derived.select(derived.column("id")).from("cq_source");
    DbQuery sourceOptions;
    sourceOptions.select(sourceOptions.star("d")).from(derived, "d", {.columns = {{.name = "id"}}});
    const auto sourceStatement = sourceOptions.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(sourceStatement.sql().find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"d\" (\"id\")") != std::string_view::npos);
    const auto mariaSourceStatement = sourceOptions.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK(mariaSourceStatement.sql().find("FROM (SELECT `id` FROM `cq_source`) AS `d` (`id`)") != std::string_view::npos);

    DbQuery recordSource;
    const auto recordFunction = recordSource.call("jsonb_to_record", {recordSource.value(R"({"id":1})")});
    recordSource.select(recordSource.star("r"))
        .fromFunction(recordFunction, "r",
            {.columns = {{.name = "id", .type = {.dataType = DbDataType::kInteger}},
                 {.name = "name", .type = {.dataType = DbDataType::kVarchar, .length = 8}}}});
    const auto recordStatement = recordSource.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(recordStatement.sql().find("AS \"r\" (\"id\" INTEGER, \"name\" VARCHAR(8))") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)recordSource.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery lateral;
    lateral.select(lateral.star("d")).from(derived, "d", {.lateral = true});
    RUVIA_CHECK(compileSql([&] { return lateral.compile(DbDriver::kPostgreSql, nullptr); }).find("FROM LATERAL") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)lateral.compile(DbDriver::kMariaDb, nullptr); }));
    DbQuery badOrdinality;
    badOrdinality.select(badOrdinality.star("d")).from(derived, "d", {.withOrdinality = true});
    RUVIA_CHECK(testing::throwsOn([&] { (void)badOrdinality.compile(DbDriver::kPostgreSql, nullptr); }));

    const std::array joins{
        std::pair{DbJoinType::kInner, std::string_view(" INNER JOIN ")},
        std::pair{DbJoinType::kLeft, std::string_view(" LEFT JOIN ")},
        std::pair{DbJoinType::kRight, std::string_view(" RIGHT JOIN ")},
        std::pair{DbJoinType::kFull, std::string_view(" FULL JOIN ")},
    };
    for (const auto& [type, token] : joins) {
        DbQuery joined;
        joined.select(joined.star("a")).from("cq_a", "a");
        const auto on = joined.binary(joined.column("id", "a"), Op::kEqual, joined.column("id", "b"));
        joined.join(type, "cq_b", on, "b");
        const auto postgres = joined.compile(DbDriver::kPostgreSql, nullptr);
        RUVIA_CHECK(postgres.sql().find(token) != std::string_view::npos);
        if (type == DbJoinType::kFull) {
            RUVIA_CHECK(testing::throwsOn([&] { (void)joined.compile(DbDriver::kMariaDb, nullptr); }));
        } else {
            const auto maria = joined.compile(DbDriver::kMariaDb, nullptr);
            RUVIA_CHECK(maria.sql().find(token) != std::string_view::npos);
        }
    }
    DbQuery cross;
    cross.select(cross.star("a")).from("cq_a", "a").join(DbJoinType::kCross, "cq_b", {}, "b");
    RUVIA_CHECK(compileSql([&] { return cross.compile(DbDriver::kPostgreSql, nullptr); }).find(" CROSS JOIN ") != std::string_view::npos);
    cross.join(DbJoinType::kCross, "cq_c", cross.column("id"), "c");
    RUVIA_CHECK(testing::throwsOn([&] { (void)cross.compile(DbDriver::kPostgreSql, nullptr); }));
}

RUVIA_TEST(db_query_dml_and_conflict_overloads_cover_postgresql_and_mariadb) {
    DbQuery insert;
    const std::array<std::string_view, 2> columns{"id", "name"};
    insert.insertInto("cq_devices", std::span<const std::string_view>(columns), "d");
    const std::array row{insert.value(1), insert.value("first")};
    insert.values(std::span<const DbQuery::Expr>(row));
    const auto inserted = insert.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(inserted.sql().find("INSERT INTO \"cq_devices\" AS \"d\"") != std::string_view::npos);
    RUVIA_CHECK(!inserted.returnsRows());

    DbQuery defaults;
    defaults.insertInto("cq_devices");
    const auto defaultPg = defaults.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(defaultPg.sql().find("DEFAULT VALUES") != std::string_view::npos);
    RUVIA_CHECK(compileSql([&] { return defaults.compile(DbDriver::kMariaDb, nullptr); }).find("() VALUES ()") != std::string_view::npos);

    DbQuery source;
    source.select({source.column("id"), source.column("name")}).from("cq_source");
    DbQuery insertFrom;
    insertFrom.insertInto("cq_devices", {"id", "name"}).insertFrom(source);
    RUVIA_CHECK(compileSql([&] { return insertFrom.compile(DbDriver::kPostgreSql, nullptr); }).find("SELECT \"id\", \"name\"") != std::string_view::npos);

    DbQuery doNothing;
    doNothing.insertInto("cq_devices", {"id", "name"}).values({doNothing.value(1), doNothing.value("x")});
    doNothing.onConflict({.columns = {"id"}, .doNothing = true});
    RUVIA_CHECK(compileSql([&] { return doNothing.compile(DbDriver::kPostgreSql, nullptr); }).find("ON CONFLICT (\"id\") DO NOTHING") != std::string_view::npos);

    DbQuery constraint;
    constraint.insertInto("cq_devices", {"id", "name"}).values({constraint.value(1), constraint.value("x")});
    constraint.onConflict({.constraint = "cq_devices_pkey", .doNothing = true});
    RUVIA_CHECK(compileSql([&] { return constraint.compile(DbDriver::kPostgreSql, nullptr); }).find("ON CONSTRAINT \"cq_devices_pkey\" DO NOTHING") != std::string_view::npos);

    DbQuery targeted;
    targeted.insertInto("cq_devices", {"id", "name"}).values({targeted.value(1), targeted.value("x")});
    const auto active = targeted.binary(targeted.column("active"), Op::kEqual, targeted.value(true));
    targeted.onConflict({.columns = {"id"}, .targetWhere = active, .update = {{"name", targeted.excluded("name")}}, .updateWhere = targeted.binary(targeted.column("name"), Op::kNotEqual, targeted.excluded("name"))});
    const auto targetedStatement = targeted.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(targetedStatement.sql().find("ON CONFLICT (\"id\") WHERE (\"active\" = $3) DO UPDATE SET") != std::string_view::npos);
    RUVIA_CHECK(targetedStatement.sql().find("WHERE (\"name\" <> ") != std::string_view::npos);
    RUVIA_CHECK(targetedStatement.sql().find("excluded.\"name\")") != std::string_view::npos ||
                targetedStatement.sql().find("\"excluded\".\"name\")") != std::string_view::npos);

    DbQuery mariaUpsert;
    mariaUpsert.insertInto("cq_devices", {"id", "name"}).values({mariaUpsert.value(1), mariaUpsert.value("x")});
    mariaUpsert.onConflict({.update = {{"name", mariaUpsert.excluded("name")}}, .anyUniqueKey = true});
    const auto mariaStatement = mariaUpsert.compile(DbDriver::kMariaDb, nullptr);
    RUVIA_CHECK(mariaStatement.sql().find("ON DUPLICATE KEY UPDATE \"name\"") == std::string_view::npos);
    RUVIA_CHECK(mariaStatement.sql().find("ON DUPLICATE KEY UPDATE `name` = VALUES(`name`)") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)mariaUpsert.compile(DbDriver::kPostgreSql, nullptr); }));
    DbQuery invalidConflict;
    invalidConflict.insertInto("cq_devices", {"id"}).values({invalidConflict.value(1)});
    invalidConflict.onConflict({.columns = {"id"}, .constraint = "other", .doNothing = true});
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalidConflict.compile(DbDriver::kPostgreSql, nullptr); }));

    DbQuery update;
    update.update("cq_devices", "d").set("name", update.value("updated"));
    update.set("name", update.value("updated-again"));
    DbQuery updateFrom;
    updateFrom.select(updateFrom.column("id")).from("cq_source");
    update.updateFrom(updateFrom, "s").where(update.binary(update.column("id", "d"), Op::kEqual, update.column("id", "s")));
    RUVIA_CHECK(compileSql([&] { return update.compile(DbDriver::kPostgreSql, nullptr); }).find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);

    DbQuery remove;
    remove.deleteFrom("cq_devices", "d");
    DbQuery deleteSource;
    deleteSource.select(deleteSource.column("id")).from("cq_source");
    remove.deleteUsing(deleteSource, "s").where(remove.binary(remove.column("id", "d"), Op::kEqual, remove.column("id", "s")));
    RUVIA_CHECK(compileSql([&] { return remove.compile(DbDriver::kPostgreSql, nullptr); }).find("USING (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)remove.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery returning;
    returning.insertInto("cq_devices", {"id"}).values({returning.value(1)}).returning({returning.column("id")});
    const auto returningStatement = returning.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(returningStatement.returnsRows());
    RUVIA_CHECK(testing::throwsOn([&] { (void)returning.compile(DbDriver::kMariaDb, nullptr); }));
}

RUVIA_TEST(db_query_lock_cte_and_set_operation_enumerations_cover_supported_and_rejected_paths) {
    struct LockCase final {
        DbRowLock mode;
        std::string_view postgresToken;
        std::string_view mariaToken;
    };
    const std::array locks{
        LockCase{DbRowLock::kUpdate, "FOR UPDATE", "FOR UPDATE"},
        LockCase{DbRowLock::kNoKeyUpdate, "FOR NO KEY UPDATE", {}},
        LockCase{DbRowLock::kShare, "FOR SHARE", "LOCK IN SHARE MODE"},
        LockCase{DbRowLock::kKeyShare, "FOR KEY SHARE", {}},
    };
    for (const auto& item : locks) {
        DbQuery query;
        query.select(query.star("d")).from("cq_devices", "d").lock({.mode = item.mode});
        const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.postgresToken) != std::string_view::npos);
        if (item.mariaToken.empty()) {
            RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));
        } else {
            RUVIA_CHECK(compileSql([&] { return query.compile(DbDriver::kMariaDb, nullptr); }).find(item.mariaToken) != std::string_view::npos);
        }
    }
    DbQuery scopedLock;
    scopedLock.select(scopedLock.star("d")).from("cq_devices", "d").lock({.mode = DbRowLock::kUpdate, .nowait = true, .tables = {"d"}});
    RUVIA_CHECK(compileSql([&] { return scopedLock.compile(DbDriver::kPostgreSql, nullptr); }).find("FOR UPDATE OF \"d\" NOWAIT") != std::string_view::npos);
    DbQuery skipLocked;
    skipLocked.select(skipLocked.star()).from("cq_devices").lock({.skipLocked = true});
    RUVIA_CHECK(compileSql([&] { return skipLocked.compile(DbDriver::kPostgreSql, nullptr); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    RUVIA_CHECK(compileSql([&] { return skipLocked.compile(DbDriver::kMariaDb, nullptr); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    DbQuery cleared;
    cleared.select(cleared.star()).from("cq_devices").lock({}).clearLock();
    RUVIA_CHECK(compileSql([&] { return cleared.compile(DbDriver::kPostgreSql, nullptr); }).find(" FOR ") == std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { cleared.lock({.nowait = true, .skipLocked = true}); }));
    DbQuery sharedSkip;
    sharedSkip.select(sharedSkip.star()).from("cq_devices").lock({.mode = DbRowLock::kShare, .skipLocked = true});
    RUVIA_CHECK(testing::throwsOn([&] { (void)sharedSkip.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery cteBody;
    cteBody.select(cteBody.value(1)).from("cq_seed");
    DbQuery recursive;
    recursive.with("numbers", cteBody, {.recursive = true, .materialization = DbMaterialization::kNotMaterialized, .columns = {"n"}})
        .select(recursive.column("n"))
        .from("numbers");
    const auto recursiveStatement = recursive.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(recursiveStatement.sql().find("WITH RECURSIVE \"numbers\" (\"n\") AS NOT MATERIALIZED") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)recursive.compile(DbDriver::kMariaDb, nullptr); }));
    DbQuery duplicateCte;
    duplicateCte.with("one", cteBody);
    RUVIA_CHECK(testing::throwsOn([&] { duplicateCte.with("one", cteBody); }));

    struct SetCase final {
        DbSetOperation operation;
        std::string_view postgresToken;
        bool mariaSupported;
    };
    const std::array setCases{
        SetCase{DbSetOperation::kUnion, " UNION ", true},
        SetCase{DbSetOperation::kUnionAll, " UNION ALL ", true},
        SetCase{DbSetOperation::kIntersect, " INTERSECT ", true},
        SetCase{DbSetOperation::kIntersectAll, " INTERSECT ALL ", false},
        SetCase{DbSetOperation::kExcept, " EXCEPT ", true},
        SetCase{DbSetOperation::kExceptAll, " EXCEPT ALL ", false},
    };
    for (const auto& item : setCases) {
        DbQuery first;
        first.select(first.value(1));
        DbQuery second;
        second.select(second.value(2));
        first.combine(item.operation, second);
        RUVIA_CHECK(compileSql([&] { return first.compile(DbDriver::kPostgreSql, nullptr); }).find(item.postgresToken) != std::string_view::npos);
        if (item.mariaSupported) {
            RUVIA_CHECK(compileSql([&] { return first.compile(DbDriver::kMariaDb, nullptr); }).find(item.postgresToken) != std::string_view::npos);
        } else {
            RUVIA_CHECK(testing::throwsOn([&] { (void)first.compile(DbDriver::kMariaDb, nullptr); }));
        }
    }
}

RUVIA_TEST(db_query_window_frame_and_date_part_enumerations_cover_each_value) {
    const std::array frames{DbWindowFrame::kRows, DbWindowFrame::kRange, DbWindowFrame::kGroups};
    for (const auto frame : frames) {
        DbQuery query;
        query.select(query.over(query.call("row_number"),
                         {.frame = DbWindowFrameOptions{.kind = frame,
                              .start = {DbFrameBoundary::kUnboundedPreceding, 0},
                              .end = {DbFrameBoundary::kCurrentRow, 0}}}))
            .from("cq_window");
        const auto postgres = query.compile(DbDriver::kPostgreSql, nullptr);
        const auto token = frame == DbWindowFrame::kRows ? "ROWS" : frame == DbWindowFrame::kRange ? "RANGE"
                                                                                                   : "GROUPS";
        RUVIA_CHECK(postgres.sql().find(token) != std::string_view::npos);
        if (frame == DbWindowFrame::kGroups) {
            RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));
        } else {
            RUVIA_CHECK(compileSql([&] { return query.compile(DbDriver::kMariaDb, nullptr); }).find(token) != std::string_view::npos);
        }
    }

    const std::array boundaries{
        std::pair{DbFrameBoundary::kUnboundedPreceding, std::string_view("UNBOUNDED PRECEDING")},
        std::pair{DbFrameBoundary::kPreceding, std::string_view("2 PRECEDING")},
        std::pair{DbFrameBoundary::kCurrentRow, std::string_view("CURRENT ROW")},
        std::pair{DbFrameBoundary::kFollowing, std::string_view("3 FOLLOWING")},
        std::pair{DbFrameBoundary::kUnboundedFollowing, std::string_view("UNBOUNDED FOLLOWING")},
    };
    for (const auto& [boundary, token] : boundaries) {
        DbQuery query;
        const auto end = boundary == DbFrameBoundary::kUnboundedFollowing ? boundary : DbFrameBoundary::kUnboundedFollowing;
        const auto start = boundary == DbFrameBoundary::kUnboundedFollowing ? DbFrameBoundary::kCurrentRow : boundary;
        const auto startValue = start == DbFrameBoundary::kPreceding ? 2U : start == DbFrameBoundary::kFollowing ? 3U
                                                                                                                 : 0U;
        const auto endValue = end == DbFrameBoundary::kPreceding ? 2U : end == DbFrameBoundary::kFollowing ? 3U
                                                                                                           : 0U;
        query.select(query.over(query.call("row_number"),
                         {.frame = DbWindowFrameOptions{.kind = DbWindowFrame::kRows,
                              .start = {start, startValue},
                              .end = {end, endValue}}}))
            .from("cq_window");
        RUVIA_CHECK(compileSql([&] { return query.compile(DbDriver::kPostgreSql, nullptr); }).find(token) != std::string_view::npos);
    }
    DbQuery invalidBoundary;
    invalidBoundary.select(invalidBoundary.over(invalidBoundary.call("row_number"),
                               {.frame = DbWindowFrameOptions{.start = {DbFrameBoundary::kCurrentRow, 1}}}))
        .from("cq_window");
    RUVIA_CHECK(testing::throwsOn([&] { (void)invalidBoundary.compile(DbDriver::kPostgreSql, nullptr); }));
    DbQuery reversed;
    reversed.select(reversed.over(reversed.call("row_number"),
                        {.frame = DbWindowFrameOptions{.start = {DbFrameBoundary::kFollowing, 0},
                             .end = {DbFrameBoundary::kPreceding, 0}}}))
        .from("cq_window");
    RUVIA_CHECK(testing::throwsOn([&] { (void)reversed.compile(DbDriver::kPostgreSql, nullptr); }));

    struct DateCase final {
        DbDatePart part;
        std::string_view token;
        bool mariaSupported;
    };
    const std::array dates{
        DateCase{DbDatePart::kEpoch, "EPOCH", false},
        DateCase{DbDatePart::kYear, "YEAR", true},
        DateCase{DbDatePart::kMonth, "MONTH", true},
        DateCase{DbDatePart::kDay, "DAY", true},
        DateCase{DbDatePart::kHour, "HOUR", true},
        DateCase{DbDatePart::kMinute, "MINUTE", true},
        DateCase{DbDatePart::kSecond, "SECOND", true},
        DateCase{DbDatePart::kDow, "DOW", false},
        DateCase{DbDatePart::kDoy, "DOY", false},
        DateCase{DbDatePart::kWeek, "WEEK", true},
        DateCase{DbDatePart::kQuarter, "QUARTER", true},
    };
    for (const auto& item : dates) {
        DbQuery query;
        query.select(query.extract(item.part, query.column("created_at"))).from("cq_window");
        RUVIA_CHECK(compileSql([&] { return query.compile(DbDriver::kPostgreSql, nullptr); }).find(item.token) != std::string_view::npos);
        if (item.mariaSupported) {
            RUVIA_CHECK(compileSql([&] { return query.compile(DbDriver::kMariaDb, nullptr); }).find(item.token) != std::string_view::npos);
        } else {
            RUVIA_CHECK(testing::throwsOn([&] { (void)query.compile(DbDriver::kMariaDb, nullptr); }));
        }
    }
}

RUVIA_TEST(db_query_expression_ownership_import_subquery_cast_and_cache_methods_are_observable) {
    DbQuery source;
    const auto qualified = source.column("id", "source");
    DbQuery target;
    const auto remapped = target.importExpression(qualified, "source", "target");
    target.select(remapped).from("cq_devices", "target");
    RUVIA_CHECK_EQ(compileSql([&] { return target.compile(DbDriver::kPostgreSql, nullptr); }), "SELECT \"target\".\"id\" FROM \"cq_devices\" AS \"target\"");
    RUVIA_CHECK(testing::throwsOn([&] { (void)target.importExpression(qualified, "source", {}); }));
    RUVIA_CHECK(testing::throwsOn([&] { (void)target.importExpression({}, "source", "target"); }));

    DbQuery child;
    child.select(child.value(1)).from("cq_devices");
    DbQuery nested;
    const auto exists = nested.exists(child);
    const auto scalar = nested.subquery(child);
    nested.select({exists, scalar}).from("cq_devices");
    const auto nestedStatement = nested.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(nestedStatement.sql().find("EXISTS (SELECT $1 FROM \"cq_devices\")") != std::string_view::npos);
    RUVIA_CHECK(nestedStatement.sql().find("(SELECT $2 FROM \"cq_devices\")") != std::string_view::npos);
    DbQuery dml;
    dml.deleteFrom("cq_devices").returning({dml.column("id")});
    RUVIA_CHECK(testing::throwsOn([&] { (void)nested.subquery(dml); }));

    DbQuery typed;
    const auto number = typed.cast(typed.value(1), DbDataType::kInteger);
    const auto sized = typed.cast(typed.value("x"), DbTypeDefinition{.dataType = DbDataType::kVarchar, .length = 8});
    typed.select({number, sized}).from("cq_devices");
    const auto typedPg = typed.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(typedPg.sql().find("CAST($1 AS INTEGER)") != std::string_view::npos);
    RUVIA_CHECK(typedPg.sql().find("CAST($2 AS VARCHAR(8))") != std::string_view::npos);
    DbQuery namedType;
    namedType.select(namedType.cast(namedType.value(1), DbTypeDefinition{.customName = "cq_custom_type"})).from("cq_devices");
    RUVIA_CHECK(compileSql([&] { return namedType.compile(DbDriver::kPostgreSql, nullptr); }).find("CAST($1 AS \"cq_custom_type\")") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)namedType.compile(DbDriver::kMariaDb, nullptr); }));

    DbQuery named;
    const std::array<DbNamedArgument, 1> namedArguments{{DbNamedArgument{"precision", named.value(2)}}};
    named.select(named.call("round", {}, namedArguments)).from("cq_devices");
    RUVIA_CHECK(compileSql([&] { return named.compile(DbDriver::kPostgreSql, nullptr); }).find("\"precision\" => $1") != std::string_view::npos);
    RUVIA_CHECK(testing::throwsOn([&] { (void)named.compile(DbDriver::kMariaDb, nullptr); }));
    RUVIA_CHECK(testing::throwsOn([&] {
        const std::array<DbNamedArgument, 2> duplicate{{DbNamedArgument{"x", named.value(1)}, DbNamedArgument{"x", named.value(2)}}};
        (void)named.call("round", {}, duplicate);
    }));

    DbQuery state;
    RUVIA_CHECK(state.resource() != nullptr);
    RUVIA_CHECK(state.returnsRows());
    RUVIA_CHECK(!state.hasWhere());
    RUVIA_CHECK(!state.hasGrouping());
    state.cache(false).cache(std::chrono::milliseconds(50)).cache(DbCacheOptions{.id = "cq-cache", .milliseconds = std::chrono::milliseconds(25)});
    state.select(state.value(1)).from("cq_devices");
    const auto stateStatement = state.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(stateStatement.params().size() == std::size_t{1});
    RUVIA_CHECK(testing::throwsOn([&] { state.cache("bad", std::chrono::milliseconds(0)); }));
    RUVIA_CHECK(testing::throwsOn([&] { state.cache("bad", std::chrono::milliseconds(-1)); }));

    std::pmr::unsynchronized_pool_resource clonedResource;
    auto clone = state.clone(&clonedResource);
    const auto cloneSql = compileSql([&] { return clone.compile(DbDriver::kPostgreSql, nullptr); });
    const auto stateSql = compileSql([&] { return state.compile(DbDriver::kPostgreSql, nullptr); });
    RUVIA_CHECK_EQ(cloneSql, stateSql);
    DbQuery moved = std::move(clone);
    RUVIA_CHECK(moved.resource() == &clonedResource);
    RUVIA_CHECK(testing::throwsOn([&] { (void)clone.resource(); }));
    DbQuery assigned;
    assigned = std::move(moved);
    RUVIA_CHECK(assigned.returnsRows());
    RUVIA_CHECK(testing::throwsOn([&] { (void)moved.returnsRows(); }));

    DbQuery values;
    values.values({values.value(1), values.value(2)});
    RUVIA_CHECK(values.returnsRows());
    DbQuery dmlInsert;
    dmlInsert.insertInto("cq_devices", {"id"}).values({dmlInsert.value(1)});
    RUVIA_CHECK(!dmlInsert.returnsRows());
    dmlInsert.returning({dmlInsert.column("id")});
    RUVIA_CHECK(dmlInsert.returnsRows());
}

RUVIA_TEST(db_query_span_overloads_cover_expression_and_statement_builders) {
    DbQuery query;
    const auto left = query.column("left");
    const auto right = query.column("right");
    const std::array<DbQuery::Expr, 2> expressions{left, right};
    const auto call = query.call("coalesce", std::span<const DbQuery::Expr>(expressions));
    const auto aggregate = query.aggregate("count", std::span<const DbQuery::Expr>(expressions));
    const auto coalesced = query.coalesce(std::span<const DbQuery::Expr>(expressions));
    const auto greatest = query.greatest(std::span<const DbQuery::Expr>(expressions));
    const auto least = query.least(std::span<const DbQuery::Expr>(expressions));
    const auto tuple = query.tuple(std::span<const DbQuery::Expr>(expressions));
    const auto list = query.list(std::span<const DbQuery::Expr>(expressions));
    const auto array = query.array(std::span<const DbQuery::Expr>(expressions));
    const std::array<DbCaseBranch, 1> branches{{{query.binary(left, DbBinaryOperator::kEqual, query.value(1)), right}}};
    const auto caseExpression = query.caseWhen(std::span<const DbCaseBranch>(branches), query.value(0));
    const std::array<DbOrderTerm, 1> order{{{left, DbOrderDirection::kAsc, DbNullsOrder::kDefault}}};
    const auto within = query.withinGroup(aggregate, std::span<const DbOrderTerm>(order));
    query.select(std::span<const DbQuery::Expr>(expressions)).addSelect(call).from("cq_span").groupBy(std::span<const DbQuery::Expr>(&left, 1)).addGroupBy(right).having(query.unary(DbUnaryOperator::kIsNotNull, coalesced)).andHaving(query.binary(greatest, DbBinaryOperator::kGreater, least)).orderBy(left).addOrderBy(right);
    const auto statement = query.compile(DbDriver::kPostgreSql, nullptr);
    RUVIA_CHECK(statement.sql().find("GROUP BY") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("HAVING") != std::string_view::npos);

    DbQuery distinct;
    const auto distinctLeft = distinct.column("left");
    distinct.select(distinctLeft);
    const std::array<DbQuery::Expr, 1> distinctExpressions{distinct.column("left")};
    distinct.distinctOn(std::span<const DbQuery::Expr>(distinctExpressions)).from("cq_span");
    RUVIA_CHECK(compileSql([&] { return distinct.compile(DbDriver::kPostgreSql, nullptr); }).find("DISTINCT ON") != std::string_view::npos);

    DbQuery dml;
    const std::array<std::string_view, 1> columns{"value"};
    const std::array<DbQuery::Expr, 1> row{dml.value(1)};
    dml.insertInto("cq_span", std::span<const std::string_view>(columns)).values(std::span<const DbQuery::Expr>(row));
    const std::array<DbQuery::Expr, 1> returned{dml.column("value")};
    dml.returning(std::span<const DbQuery::Expr>(returned));
    RUVIA_CHECK(dml.compile(DbDriver::kPostgreSql, nullptr).returnsRows());

    (void)tuple;
    (void)list;
    (void)array;
    (void)caseExpression;
    (void)within;
}

}  // namespace
