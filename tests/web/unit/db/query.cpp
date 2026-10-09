#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_predicate.h"
#include "ruvia/web/detail/db/db_value_access.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;
using op_type = db_binary_operator;
using entity_type = db_entity<"device", db_column<"id", std::int64_t>, db_column<"name", std::pmr::string>>;

RUVIA_TEST(db_query_sql_expression_import_preserves_syntax_and_parameter_binding) {
    test::counting_memory_resource source_resource, target_resource;
    {
        db_query target(&target_resource);
        {
            db_query source_value(&source_resource);
            auto expression = source_value.sql({"jsonb_set(", ", '{label}', to_jsonb(", "::text))"},
                {source_value.column("payload"), source_value.value("x'); DELETE FROM device; --")});
            target.update("device").set("payload", target.import_expression(expression));
            target.returning({target.column("id")});
        }
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        auto statement = target.compile(db_driver::postgresql, &target_resource);
        RUVIA_CHECK_EQ(statement.sql(), "UPDATE \"device\" SET \"payload\" = (jsonb_set(\"payload\", '{label}', to_jsonb($1::text))) RETURNING \"id\"");
        RUVIA_CHECK_EQ(statement.params().size(), std::size_t{1});
        RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[0]), "x'); DELETE FROM device; --");
        RUVIA_CHECK(testing::throws_on([&] { (void)target.sql({"one"}, {target.value(1)}); }));
        RUVIA_CHECK(testing::throws_on([&] { (void)target.sql(std::string_view("x\0y", 3)); }));
    }
    RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
}

template <typename fn_type>
std::string compile_sql(fn_type&& fn) {
    auto statement = fn();
    return std::string(statement.sql());
}

RUVIA_TEST(db_query_reuses_postgresql_parameters_for_grouped_expressions) {
    db_query query;
    const auto bucket = query.binary(query.column("id"), op_type::add, query.value(1));
    query.select({bucket, query.aggregate("count", {query.star()})}).from("events").group_by({bucket});
    const auto postgres = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(postgres.sql(), "SELECT (\"id\" + $1), \"count\"(*) FROM \"events\" GROUP BY (\"id\" + $1)");
    RUVIA_CHECK_EQ(postgres.params().size(), std::size_t{1});
    const auto maria = query.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK_EQ(maria.params().size(), std::size_t{2});
}

RUVIA_TEST(db_query_binds_values_in_wire_order_and_preserves_zero_limit) {
    db_query query;
    const auto second = query.value(db_value("O'Reilly $1 ?"));
    const auto first = query.value(db_value(9));
    const auto id = query.column("id", "u");
    query.select(first).from("users", "u").where(query.binary(id, op_type::equal, second));
    query.order_by(id, db_order_direction::desc).limit(0).lock({.nowait_ = true});
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT $1 FROM \"users\" AS \"u\" WHERE (\"u\".\"id\" = $2) ORDER BY \"u\".\"id\" DESC LIMIT $3 FOR UPDATE NOWAIT");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
    RUVIA_CHECK_EQ(detail::db_value_access::signed_value(statement.params()[0]), 9);
    RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[1]), "O'Reilly $1 ?");
    RUVIA_CHECK_EQ(detail::db_value_access::signed_value(statement.params()[2]), 0);
}

RUVIA_TEST(db_query_nested_ctes_are_owned_and_use_one_parameter_sequence) {
    db_query query;
    {
        db_query incoming;
        const std::array row{incoming.value(db_value(7)), incoming.value(db_value("first"))};
        incoming.values(row);
        query.with("incoming", incoming, {.materialization_ = db_materialization::materialized, .columns_ = {"id", "name"}});
        incoming.values(row);
    }
    db_query filter;
    filter.select(filter.column("id")).from("incoming").where(filter.binary(filter.column("name"), op_type::equal, filter.value(db_value("first"))));
    query.select(query.value(db_value(11))).from(filter, "filtered");
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "WITH \"incoming\" (\"id\", \"name\") AS MATERIALIZED (VALUES ($1, $2)) SELECT $3 FROM (SELECT \"id\" FROM \"incoming\" WHERE (\"name\" = $4)) AS \"filtered\"");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{4});
    RUVIA_CHECK_EQ(detail::db_value_access::signed_value(statement.params()[2]), 11);
}

RUVIA_TEST(db_query_null_comparisons_and_empty_membership_have_defined_semantics) {
    db_query query;
    const auto id = query.column("id");
    const std::array projections{query.binary(id, op_type::equal, query.null_value()), query.binary(query.null_value(), op_type::not_equal, id),
        query.binary(id, op_type::in, query.list({})), query.binary(id, op_type::not_in, query.list({}))};
    query.select(projections).from("devices");
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT (\"id\" IS NULL), (\"id\" IS NOT NULL), FALSE, TRUE FROM \"devices\"");
    RUVIA_CHECK(statement.params().empty());
}

RUVIA_TEST(db_query_window_aggregate_filter_and_lateral_source) {
    db_query query;
    auto ts = query.column("ts", "e");
    auto id = query.column("id", "e");
    auto rank = query.over(query.call("row_number"), {.partition_by_ = {id}, .order_by_ = {{ts, db_order_direction::desc}}});
    const std::array args{id};
    const std::array order{db_order_term{ts, db_order_direction::asc}};
    auto aggregate = query.filter(query.aggregate("array_agg", args, false, order), query.unary(db_unary_operator::is_not_null, id));
    const std::array projections{query.alias(rank, "rank"), aggregate};
    query.select(projections).from("events", "e");
    query.join_function(db_join_type::cross, query.call("jsonb_each", {query.column("data", "e")}), {}, "kv", {.lateral_ = true});
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(statement.sql().find("\"row_number\"() OVER (PARTITION BY \"e\".\"id\" ORDER BY \"e\".\"ts\" DESC)") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("\"array_agg\"(\"e\".\"id\" ORDER BY \"e\".\"ts\" ASC) FILTER (WHERE (\"e\".\"id\" IS NOT NULL))") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("CROSS JOIN LATERAL \"jsonb_each\"(\"e\".\"data\") AS \"kv\"") != std::string_view::npos);
}

RUVIA_TEST(db_query_expression_helpers_render_postgresql_and_reject_empty_inputs) {
    db_query query;
    const auto left = query.column("left");
    const auto right = query.column("right");
    const auto values = query.array({query.value(1), query.value(2)});
    const std::array ordered_by{db_order_term{query.column("score"), db_order_direction::desc}};
    const auto ordered = query.within_group(
        query.aggregate("percentile_cont", {query.value(0.5)}),
        ordered_by);
    const auto branches = std::array{
        db_case_branch{query.binary(left, op_type::greater, query.value(0)), query.value("positive")}};
    const std::array<db_query::expr_type, 12> projections{
        query.coalesce({query.null_value(), left}),
        query.null_if(left, right),
        query.greatest({left, right}),
        query.least({left, right}),
        query.tuple({left, right}),
        query.any(values),
        query.all(values),
        query.case_when(branches, query.value("zero")),
        ordered,
        query.extract(db_date_part::year, query.column("created_at")),
        query.subscript(query.column("tags"), query.value(1)),
        query.collate(query.column("name"), "C"),
    };
    query.select(projections).from("metrics");

    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(),
        "SELECT COALESCE(NULL, \"left\"), NULLIF(\"left\", \"right\"), GREATEST(\"left\", \"right\"), LEAST(\"left\", \"right\"), ROW(\"left\", \"right\"), ANY(ARRAY[$1, $2]), ALL(ARRAY[$1, $2]), CASE WHEN (\"left\" > $3) THEN $4 ELSE $5 END, \"percentile_cont\"($6) WITHIN GROUP (ORDER BY \"score\" DESC), EXTRACT(YEAR FROM \"created_at\"), (\"tags\")[$7], (\"name\" COLLATE \"C\") FROM \"metrics\"");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{7});

    db_query maria_array;
    const auto array_value = maria_array.array({maria_array.value(1)});
    maria_array.select(maria_array.any(array_value)).from("metrics");
    RUVIA_CHECK(testing::throws_on([&] { (void)maria_array.compile(db_driver::mariadb, nullptr); }));

    db_query maria_within_group;
    const auto aggregate = maria_within_group.aggregate("percentile_cont", {maria_within_group.value(0.5)});
    const std::array maria_order{db_order_term{maria_within_group.column("score"), db_order_direction::asc}};
    maria_within_group.select(maria_within_group.within_group(aggregate, maria_order))
        .from("metrics");
    RUVIA_CHECK(testing::throws_on([&] { (void)maria_within_group.compile(db_driver::mariadb, nullptr); }));

    RUVIA_CHECK(testing::throws_on([&] { (void)query.coalesce({}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)query.greatest({}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)query.least({}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)query.within_group(left, {}); }));
    db_query empty_tuple;
    empty_tuple.select(empty_tuple.tuple({})).from("metrics");
    RUVIA_CHECK(testing::throws_on([&] { (void)empty_tuple.compile(db_driver::postgresql, nullptr); }));
}

RUVIA_TEST(db_query_native_functions_and_identifier_quoting_are_dialect_safe) {
    db_query query;
    const auto amount = query.column("amount", "s");
    const auto name = query.column("name", "s");
    const auto count = query.aggregate("count", {query.star()});
    const auto sum = query.aggregate("sum", {amount});
    const auto row_number = query.over(query.call("row_number"),
        {.order_by_ = {{query.column("id", "s"), db_order_direction::asc}}});
    const auto scalar = query.call("lower", {name});
    query.select({count, sum, row_number, scalar}).from("sales", "s");

    const auto postgres = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(postgres.sql(),
        "SELECT \"count\"(*), \"sum\"(\"s\".\"amount\"), \"row_number\"() OVER (ORDER BY \"s\".\"id\" ASC), \"lower\"(\"s\".\"name\") FROM \"sales\" AS \"s\"");

    const auto maria = query.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(maria.sql().find("count(*)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("sum(`s`.`amount`)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("row_number() OVER (ORDER BY `s`.`id` ASC)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("lower(`s`.`name`)") != std::string_view::npos);
    RUVIA_CHECK(maria.sql().find("`count`") == std::string_view::npos);

    db_query identifiers;
    const auto dangerous_column = identifiers.column("name\"; DROP TABLE users --", "tenant.schema");
    const auto dangerous_function = identifiers.call("pkg.weird\"function", {dangerous_column});
    const auto special_function = identifiers.call("weird`fn--", {dangerous_column});
    identifiers.select({dangerous_function, special_function}).from("tenant.schema", "s");
    const auto safe_postgres = identifiers.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(safe_postgres.sql().find("\"tenant\".\"schema\".\"name\"\"; DROP TABLE users --\"") != std::string_view::npos);
    RUVIA_CHECK(safe_postgres.sql().find("\"pkg\".\"weird\"\"function\"") != std::string_view::npos);
    const auto safe_maria = identifiers.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(safe_maria.sql().find("`tenant`.`schema`.`name\"; DROP TABLE users --`") != std::string_view::npos);
    RUVIA_CHECK(safe_maria.sql().find("`pkg`.`weird\"function`") != std::string_view::npos);
    RUVIA_CHECK(safe_maria.sql().find("`weird``fn--`") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)identifiers.column(std::string_view("bad\0name", 8)); }));

    const std::string overlong(65, 'x');
    db_query overlong_query;
    overlong_query.select(overlong_query.call(overlong)).from("events");
    RUVIA_CHECK(testing::throws_on([&] { (void)overlong_query.compile(db_driver::postgresql, nullptr); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)overlong_query.compile(db_driver::mariadb, nullptr); }));
}

RUVIA_TEST(db_query_dml_sources_distinct_and_query_joins_compile) {
    db_query source;
    source.select({source.column("id", "s"), source.column("name", "s")}).from("source", "s");

    db_query insert;
    insert.insert_into("archive", {"id", "name"}).insert_from(source).returning({insert.column("id")});
    const auto inserted = insert.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(inserted.sql(),
        "INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\" RETURNING \"id\"");

    db_query update;
    update.update("target", "t").set("name", update.value("changed"));
    update.update_from("source", "s").where(update.binary(update.column("id", "t"), op_type::equal, update.column("id", "s")));
    const auto updated = update.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(updated.sql(),
        "UPDATE \"target\" AS \"t\" SET \"name\" = $1 FROM \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query removed;
    removed.delete_from("target", "t").delete_using("source", "s");
    removed.where(removed.binary(removed.column("id", "t"), op_type::equal, removed.column("id", "s")));
    const auto deleted = removed.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(deleted.sql(),
        "DELETE FROM \"target\" AS \"t\" USING \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query distinct;
    distinct.select({distinct.column("account_id"), distinct.column("id")})
        .distinct_on({distinct.column("account_id")})
        .from("events");
    const auto distinct_statement = distinct.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(distinct_statement.sql(),
        "SELECT DISTINCT ON (\"account_id\") \"account_id\", \"id\" FROM \"events\"");
    RUVIA_CHECK(testing::throws_on([&] { (void)distinct.compile(db_driver::mariadb, nullptr); }));

    db_query joined;
    joined.select(joined.star("base")).from("base", "base");
    const auto join_on = joined.binary(joined.column("id", "base"), op_type::equal, joined.column("id", "derived"));
    joined.join(db_join_type::left, source, join_on, "derived");
    const std::array<std::string_view, 1> using_columns{"id"};
    joined.join(db_join_type::inner, "same_ids", {}, "same_ids", using_columns);
    const auto joined_statement = joined.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(joined_statement.sql(),
        "SELECT \"base\".* FROM \"base\" AS \"base\" LEFT JOIN (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\") AS \"derived\" ON (\"base\".\"id\" = \"derived\".\"id\") INNER JOIN \"same_ids\" AS \"same_ids\" USING (\"id\")");

    db_query function_source;
    const auto function = function_source.call("jsonb_each", {function_source.column("payload")});
    function_source.select(function_source.star("kv"))
        .from_function(function, "kv",
            {.lateral_ = true, .with_ordinality_ = true, .columns_ = {{.name_ = "key"}, {.name_ = "value"}}});
    const auto function_statement = function_source.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(function_statement.sql().find(
                    "FROM LATERAL \"jsonb_each\"(\"payload\") WITH ORDINALITY AS \"kv\" (\"key\", \"value\")") != std::string_view::npos);
}

RUVIA_TEST(db_query_bulk_upsert_conditions_and_returning) {
    db_query query;
    const std::array<std::string_view, 2> columns{"id", "revision"};
    query.insert_into("device", columns, "d");
    const std::array first{query.value(db_value(7)), query.value(db_value(2))};
    const std::array second{query.value(db_value(8)), query.default_value()};
    query.values(first).values(second);
    const auto revision = query.column("revision", "excluded");
    query.on_conflict({.columns_ = {"id"}, .update_ = {{"revision", revision}}, .update_where_ = query.binary(revision, op_type::greater, query.column("revision", "d"))});
    const std::array returned{query.column("id")};
    query.returning(returned);
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "INSERT INTO \"device\" AS \"d\" (\"id\", \"revision\") VALUES ($1, $2), ($3, DEFAULT) ON CONFLICT (\"id\") DO UPDATE SET \"revision\" = \"excluded\".\"revision\" WHERE (\"excluded\".\"revision\" > \"d\".\"revision\") RETURNING \"id\"");
    RUVIA_CHECK(statement.returns_rows());
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{3});
}

RUVIA_TEST(db_query_set_operations_preserve_grouping_and_nested_limits) {
    db_query first, second, third;
    first.select(first.value(db_value(1)));
    second.select(second.value(db_value(2))).limit(1);
    third.select(third.value(db_value(3)));
    first.combine(db_set_operation::union_all, second).combine(db_set_operation::intersect, third).limit(4);
    const auto statement = first.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "((SELECT $1) UNION ALL (SELECT $2 LIMIT $3)) INTERSECT (SELECT $4) LIMIT $5");
    RUVIA_CHECK_EQ(statement.params().size(), std::size_t{5});
}

RUVIA_TEST(db_query_mariadb_uses_its_own_placeholders_and_null_ordering) {
    db_query query;
    const auto name = query.column("name");
    query.select(query.binary(name, op_type::concat, query.value(db_value("suffix")))).from("user");
    query.order_by(name, db_order_direction::asc, db_nulls_order::last).offset(2);
    const auto statement = query.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT CONCAT(`name`, ?) FROM `user` ORDER BY (`name` IS NULL) ASC, `name` ASC LIMIT 18446744073709551615 OFFSET ?");
    query.where(query.binary(name, op_type::i_like, query.value(db_value("needle"))));
    RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));
}

RUVIA_TEST(db_query_rejects_foreign_expressions_and_incompatible_clauses) {
    db_query first, second;
    auto foreign = first.column("id");
    RUVIA_CHECK(testing::throws_on([&] { second.where(foreign); }));
    second.where(second.import_expression(foreign)).insert_into("device");
    RUVIA_CHECK(testing::throws_on([&] { (void)second.compile(db_driver::postgresql, nullptr); }));
    db_query invalid_join;
    invalid_join.from("one").join(db_join_type::left, "two");
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid_join.compile(db_driver::postgresql, nullptr); }));
}

RUVIA_TEST(db_query_compilation_releases_temporary_allocations_and_retains_output) {
    test::counting_memory_resource input, output;
    const auto retained = [&] {
        db_query query(&input);
        std::string text(500, 'x');
        query.select(query.value(db_value(text)));
        text.assign(500, 'y');
        return query.compile(db_driver::postgresql, &output);
    }();
    RUVIA_CHECK_EQ(input.live_allocations(), std::size_t{0});
    const auto baseline = output.live_allocations();
    for (int i = 0; i < 20; ++i) {
        {
            db_query query(&input);
            query.select(query.value(db_value("next")));
            const auto statement = query.compile(db_driver::postgresql, &output);
        }
        RUVIA_CHECK_EQ(input.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(output.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(detail::db_value_access::text(retained.params().front()), std::string(500, 'x'));
    RUVIA_CHECK(output.deallocation_count() > 0);
    {
        db_query invalid(&input);
        invalid.select(invalid.value(db_value("allocated"))).from("devices").join(db_join_type::inner, "missing_on");
        RUVIA_CHECK(testing::throws_on([&] { (void)invalid.compile(db_driver::postgresql, &output); }));
        RUVIA_CHECK_EQ(output.live_allocations(), baseline);
    }
    RUVIA_CHECK_EQ(input.live_allocations(), std::size_t{0});
}

RUVIA_TEST(db_query_data_modifications_require_top_level_or_cte_placement) {
    db_query removed;
    removed.delete_from("device").returning({removed.column("id")});
    db_query query;
    RUVIA_CHECK(testing::throws_on([&] { (void)query.subquery(removed); }));
    RUVIA_CHECK(testing::throws_on([&] { query.from(removed, "removed"); }));
    RUVIA_CHECK(testing::throws_on([&] { query.join(db_join_type::cross, removed, {}, "removed"); }));
    db_query insert;
    insert.insert_into("archive", {"id"});
    RUVIA_CHECK(testing::throws_on([&] { insert.insert_from(removed); }));
    query.with("removed", removed).from("removed");
    RUVIA_CHECK_EQ(query.compile(db_driver::postgresql, nullptr).returns_rows(), true);
    db_query nested;
    RUVIA_CHECK(testing::throws_on([&] { nested.from(query, "nested"); }));
    nested.with("nested", query).from("nested");
    RUVIA_CHECK(testing::throws_on([&] { (void)nested.compile(db_driver::postgresql, nullptr); }));
}

RUVIA_TEST(db_predicate_composes_typed_fields_and_owns_literal_strings) {
    std::string name(150, 'a');
    auto condition = (entity_type::column<"id">() >= 7) && (entity_type::column<"name">() == name);
    name.assign(150, 'z');
    db_query query;
    query.from("device").where(condition.expression(query));
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT * FROM \"device\" WHERE ((\"device\".\"id\" >= $1) AND (\"device\".\"name\" = $2))");
    RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[1]), std::string(150, 'a'));
}

RUVIA_TEST(db_predicate_between_and_array_operators_bind_owned_typed_values) {
    using tagged_type = db_entity<"tagged", db_column<"id", std::int64_t>,
        db_column<"tags", std::pmr::vector<std::pmr::string>>>;
    std::array<std::string, 2> input{std::string(200, 'a'), "zone-b"};
    auto condition = tagged_type::column<"id">().between(3, 8) && tagged_type::column<"tags">().array_contains(std::span<const std::string>(input));
    input[0].assign(200, 'z');
    db_query query;
    query.from("tagged").where(condition.expression(query));
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(statement.sql(), "SELECT * FROM \"tagged\" WHERE ((\"tagged\".\"id\" BETWEEN $1 AND $2) AND (\"tagged\".\"tags\" @> CAST(ARRAY[$3, $4] AS TEXT[])))");
    RUVIA_CHECK_EQ(detail::db_value_access::text(statement.params()[2]), std::string(200, 'a'));
    RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));

    db_query contained;
    auto subset = tagged_type::column<"tags">().array_contained_by<std::string_view>({});
    contained.from("tagged").where(subset.expression(contained));
    const auto empty = contained.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(empty.sql(), "SELECT * FROM \"tagged\" WHERE (\"tagged\".\"tags\" <@ CAST(ARRAY[] AS TEXT[]))");
    RUVIA_CHECK(empty.params().empty());

    using uuids_type = db_entity<"uuid_tags", db_column<"tags", std::pmr::vector<std::pmr::string>, db_column_options{.data_type_ = db_data_type::uuid}>>;
    db_query overlap;
    auto any = uuids_type::column<"tags">().array_overlap({"12345678-1234-1234-1234-123456789abc"});
    overlap.from("uuid_tags").where(any.expression(overlap));
    const auto typed = overlap.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK_EQ(typed.sql(), "SELECT * FROM \"uuid_tags\" WHERE (\"uuid_tags\".\"tags\" && CAST(ARRAY[$1] AS UUID[]))");
    using codes_type = db_entity<"codes", db_column<"values", std::pmr::vector<std::pmr::string>, db_column_options{.data_type_ = db_data_type::varchar, .length_ = 32}>>;
    db_query codes;
    auto code = codes_type::column<"values">().array_contains({"zone-a"});
    codes.from("codes").where(code.expression(codes));
    const auto sized = codes.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(sized.sql().find("AS VARCHAR(32)[]") != std::string_view::npos);
}

RUVIA_TEST(db_query_binary_operator_families_cover_postgresql_and_mariadb_dialects) {
    struct binary_case final {
        op_type op_;
        std::string_view postgres_token_;
        std::string_view maria_token_;
        bool list_right_{false};
    };
    const std::array common{
        binary_case{op_type::equal, " = ", " = "},
        binary_case{op_type::not_equal, " <> ", " <> "},
        binary_case{op_type::less, " < ", " < "},
        binary_case{op_type::less_equal, " <= ", " <= "},
        binary_case{op_type::greater, " > ", " > "},
        binary_case{op_type::greater_equal, " >= ", " >= "},
        binary_case{op_type::and_value, " AND ", " AND "},
        binary_case{op_type::or_value, " OR ", " OR "},
        binary_case{op_type::add, " + ", " + "},
        binary_case{op_type::subtract, " - ", " - "},
        binary_case{op_type::multiply, " * ", " * "},
        binary_case{op_type::divide, " / ", " / "},
        binary_case{op_type::modulo, " % ", " % "},
        binary_case{op_type::concat, " || ", "CONCAT("},
        binary_case{op_type::like, " LIKE ", " LIKE "},
        binary_case{op_type::not_like, " NOT LIKE ", " NOT LIKE "},
        binary_case{op_type::in, " IN ", " IN ", true},
        binary_case{op_type::not_in, " NOT IN ", " NOT IN ", true},
        binary_case{op_type::is_distinct_from, " IS DISTINCT FROM ", "<=>"},
        binary_case{op_type::is_not_distinct_from, " IS NOT DISTINCT FROM ", "<=>"},
        binary_case{op_type::bit_and, " & ", " & "},
        binary_case{op_type::bit_or, " | ", " | "},
        binary_case{op_type::bit_xor, " # ", " ^ "},
    };
    for (const auto& item : common) {
        db_query query;
        const auto lhs = query.column("lhs");
        const auto rhs = item.list_right_ ? query.list({query.value(1), query.value(2)}) : query.column("rhs");
        query.select(query.binary(lhs, item.op_, rhs)).from("cq_operator_matrix");
        const auto postgres = query.compile(db_driver::postgresql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.postgres_token_) != std::string_view::npos);
        const auto maria = query.compile(db_driver::mariadb, nullptr);
        RUVIA_CHECK(maria.sql().find(item.maria_token_) != std::string_view::npos);
    }

    struct postgres_only_case final {
        op_type op_;
        std::string_view token_;
    };
    const std::array postgres_only{
        postgres_only_case{op_type::i_like, " ILIKE "},
        postgres_only_case{op_type::not_i_like, " NOT ILIKE "},
        postgres_only_case{op_type::json_get, " -> "},
        postgres_only_case{op_type::json_get_text, " ->> "},
        postgres_only_case{op_type::json_path, " #> "},
        postgres_only_case{op_type::json_path_text, " #>> "},
        postgres_only_case{op_type::json_contains, " @> "},
        postgres_only_case{op_type::json_contained_by, " <@ "},
        postgres_only_case{op_type::json_has_key, " ? "},
        postgres_only_case{op_type::json_has_any_key, " ?| "},
        postgres_only_case{op_type::json_has_all_keys, " ?& "},
        postgres_only_case{op_type::json_concat, " || "},
        postgres_only_case{op_type::json_delete, " - "},
        postgres_only_case{op_type::json_delete_path, " #- "},
        postgres_only_case{op_type::array_contains, " @> "},
        postgres_only_case{op_type::array_contained_by, " <@ "},
        postgres_only_case{op_type::array_overlap, " && "},
        postgres_only_case{op_type::regex, " ~ "},
        postgres_only_case{op_type::regex_insensitive, " ~* "},
        postgres_only_case{op_type::inet_contains, " >> "},
        postgres_only_case{op_type::inet_contains_or_equal, " >>= "},
        postgres_only_case{op_type::inet_contained_by, " << "},
        postgres_only_case{op_type::inet_contained_by_or_equal, " <<= "},
        postgres_only_case{op_type::inet_overlap, " && "},
    };
    for (const auto& item : postgres_only) {
        db_query query;
        query.select(query.binary(query.column("lhs"), item.op_, query.column("rhs"))).from("cq_operator_matrix");
        const auto postgres = query.compile(db_driver::postgresql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.token_) != std::string_view::npos);
        RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));
    }

    db_query nulls;
    const auto id = nulls.column("id");
    nulls.select({nulls.binary(id, op_type::equal, nulls.null_value()),
                     nulls.binary(nulls.null_value(), op_type::not_equal, id),
                     nulls.binary(id, op_type::in, nulls.list({})),
                     nulls.binary(id, op_type::not_in, nulls.list({}))})
        .from("cq_operator_matrix");
    const auto null_statement = nulls.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(null_statement.sql().find(" IS NULL") != std::string_view::npos);
    RUVIA_CHECK(null_statement.sql().find(" IS NOT NULL") != std::string_view::npos);
    RUVIA_CHECK(null_statement.sql().find("FALSE") != std::string_view::npos);
    RUVIA_CHECK(null_statement.sql().find("TRUE") != std::string_view::npos);

    db_query invalid_in;
    invalid_in.select(invalid_in.binary(invalid_in.column("id"), op_type::in, invalid_in.value(1))).from("cq_operator_matrix");
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid_in.compile(db_driver::postgresql, nullptr); }));
}

RUVIA_TEST(db_query_unary_operators_and_parameter_modes_cover_all_semantics) {
    struct unary_case final {
        db_unary_operator op_;
        std::string_view token_;
    };
    const std::array cases{
        unary_case{db_unary_operator::not_value, "NOT"},
        unary_case{db_unary_operator::negate, "-"},
        unary_case{db_unary_operator::bit_not, "~"},
        unary_case{db_unary_operator::is_null, "IS NULL"},
        unary_case{db_unary_operator::is_not_null, "IS NOT NULL"},
        unary_case{db_unary_operator::is_true, "IS TRUE"},
        unary_case{db_unary_operator::is_false, "IS FALSE"},
        unary_case{db_unary_operator::is_not_true, "IS NOT TRUE"},
        unary_case{db_unary_operator::is_not_false, "IS NOT FALSE"},
    };
    for (const auto& item : cases) {
        db_query query;
        query.select(query.unary(item.op_, query.column("value"))).from("cq_unary_matrix");
        const auto postgres = query.compile(db_driver::postgresql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.token_) != std::string_view::npos);
        const auto maria = query.compile(db_driver::mariadb, nullptr);
        RUVIA_CHECK(maria.sql().find(item.token_) != std::string_view::npos);
    }

    db_query literal;
    const auto literal_value = literal.value(db_value("O'Reilly"));
    literal.select(literal_value).from("cq_unary_matrix");
    const auto bound = literal.compile(db_driver::postgresql, nullptr, db_parameter_mode::bound);
    RUVIA_CHECK_EQ(bound.params().size(), std::size_t{1});
    const auto rendered = literal.compile(db_driver::postgresql, nullptr, db_parameter_mode::literal);
    RUVIA_CHECK_EQ(rendered.params().size(), std::size_t{0});
    RUVIA_CHECK(rendered.sql().find("E'O''Reilly'") != std::string_view::npos);
    const auto maria_rendered = literal.compile(db_driver::mariadb, nullptr, db_parameter_mode::literal);
    RUVIA_CHECK(maria_rendered.sql().find("CONVERT(X'4f275265696c6c79' USING utf8mb4)") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)db_query::render_expression(literal_value, db_driver::postgresql, nullptr, db_parameter_mode::bound); }));
    RUVIA_CHECK_EQ(db_query::render_expression(literal_value, db_driver::postgresql, nullptr), "E'O''Reilly'");

    db_query invalid;
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid.unary(db_unary_operator::not_value, {}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid.binary({}, op_type::equal, invalid.value(1)); }));
}

RUVIA_TEST(db_predicate_public_overloads_cover_comparisons_membership_nulls_and_aliases) {
    using tagged_type = db_entity<"cq_device", db_column<"id", std::int64_t>, db_column<"name", std::pmr::string>,
        db_column<"tags", std::pmr::vector<std::pmr::string>>>;
    RUVIA_CHECK_EQ(tagged_type::column<"id">().name(), std::string_view("id"));

    const auto compile = [](db_predicate predicate, db_driver driver) {
        db_query query;
        query.from("cq_device", "d").where(predicate.expression(query, "cq_device", "d"));
        return query.compile(driver, nullptr);
    };
    const auto check_comparison = [&](db_predicate predicate, std::string_view token) {
        const auto statement = compile(std::move(predicate), db_driver::postgresql);
        RUVIA_CHECK(statement.sql().find(token) != std::string_view::npos);
        RUVIA_CHECK(statement.sql().find("\"d\".\"id\"") != std::string_view::npos);
    };
    check_comparison(tagged_type::column<"id">() == 1, " = ");
    check_comparison(tagged_type::column<"id">() != 1, " <> ");
    check_comparison(tagged_type::column<"id">() < 1, " < ");
    check_comparison(tagged_type::column<"id">() <= 1, " <= ");
    check_comparison(tagged_type::column<"id">() > 1, " > ");
    check_comparison(tagged_type::column<"id">() >= 1, " >= ");

    const auto like = compile(tagged_type::column<"name">().like("A%"), db_driver::postgresql);
    RUVIA_CHECK(like.sql().find(" LIKE ") != std::string_view::npos);
    const auto ilike = compile(tagged_type::column<"name">().ilike("a%"), db_driver::postgresql);
    RUVIA_CHECK(ilike.sql().find(" ILIKE ") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)compile(tagged_type::column<"name">().ilike("a%"), db_driver::mariadb); }));
    RUVIA_CHECK(compile_sql([&] { return compile(tagged_type::column<"id">().is_null(), db_driver::postgresql); }).find(" IS NULL") != std::string_view::npos);
    RUVIA_CHECK(compile_sql([&] { return compile(tagged_type::column<"id">().is_not_null(), db_driver::postgresql); }).find(" IS NOT NULL") != std::string_view::npos);

    const std::array ids{1, 2, 3};
    const auto in_span = compile(tagged_type::column<"id">().in(std::span<const int>(ids)), db_driver::postgresql);
    RUVIA_CHECK(in_span.sql().find(" IN ($1, $2, $3)") != std::string_view::npos);
    const auto in_list = compile(tagged_type::column<"id">().in({4, 5}), db_driver::postgresql);
    RUVIA_CHECK(in_list.sql().find(" IN ($1, $2)") != std::string_view::npos);
    const std::span<const int> no_ids;
    const auto empty_in = compile(tagged_type::column<"id">().in(no_ids), db_driver::postgresql);
    RUVIA_CHECK(empty_in.sql().find("FALSE") != std::string_view::npos);

    const auto between = compile(tagged_type::column<"id">().between(2, 8), db_driver::postgresql);
    RUVIA_CHECK(between.sql().find(" BETWEEN ") != std::string_view::npos);
    const std::array<std::string_view, 2> tag_values{"one", "two"};
    const auto contains = compile(tagged_type::column<"tags">().array_contains(std::span<const std::string_view>(tag_values)), db_driver::postgresql);
    RUVIA_CHECK(contains.sql().find(" @> CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto contained = compile(tagged_type::column<"tags">().array_contained_by({"one", "two"}), db_driver::postgresql);
    RUVIA_CHECK(contained.sql().find(" <@ CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto overlap = compile(tagged_type::column<"tags">().array_overlap({"two"}), db_driver::postgresql);
    RUVIA_CHECK(overlap.sql().find(" && CAST(ARRAY[$1]") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)compile(tagged_type::column<"tags">().array_contains({"one"}), db_driver::mariadb); }));

    auto combined = (tagged_type::column<"id">() == 1) || !(tagged_type::column<"name">().like("%x%"));
    const auto combined_statement = compile(std::move(combined), db_driver::postgresql);
    RUVIA_CHECK(combined_statement.sql().find(" OR ") != std::string_view::npos);
    RUVIA_CHECK(combined_statement.sql().find("NOT") != std::string_view::npos);

    db_predicate empty;
    db_query query;
    RUVIA_CHECK(empty.empty());
    RUVIA_CHECK(empty.expression(query).empty());
    db_predicate owned = tagged_type::column<"id">() == 1;
    RUVIA_CHECK(!owned.empty());
    db_predicate moved = std::move(owned);
    db_predicate reassigned;
    reassigned = std::move(moved);
    RUVIA_CHECK(!reassigned.empty());
    db_predicate empty_and;
    db_predicate empty_or;
    RUVIA_CHECK(testing::throws_on([&] { (void)(std::move(empty_and) && (tagged_type::column<"id">() == 1)); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)(std::move(empty_or) || (tagged_type::column<"id">() == 1)); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)!std::move(empty); }));
}

RUVIA_TEST(db_query_clause_source_and_join_methods_cover_state_and_dialect_rules) {
    db_query query;
    const auto id = query.column("id", "e");
    const auto kind = query.column("kind", "e");
    query.select(query.star("e"))
        .add_select(query.alias(query.column("id", "e"), "selected_id"))
        .from("cq_events", "e")
        .where(query.binary(id, op_type::greater, query.value(0)))
        .and_where(query.binary(kind, op_type::not_equal, query.value("archived")))
        .or_where(query.binary(id, op_type::equal, query.value(99)))
        .group_by({id})
        .add_group_by(kind)
        .having(query.binary(query.aggregate("count", {query.star()}), op_type::greater, query.value(0)))
        .and_having(query.binary(kind, op_type::like, query.value("%live%")))
        .order_by(id, db_order_direction::desc, db_nulls_order::first)
        .add_order_by(kind, db_order_direction::asc, db_nulls_order::last)
        .limit(10)
        .offset(2)
        .distinct();
    RUVIA_CHECK(query.has_where());
    RUVIA_CHECK(query.has_grouping());
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(statement.sql().find("SELECT DISTINCT ") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("GROUP BY \"e\".\"id\", \"e\".\"kind\"") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("HAVING") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("NULLS FIRST") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("NULLS LAST") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("LIMIT") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("OFFSET") != std::string_view::npos);

    query.clear_order().limit(std::nullopt).offset(std::nullopt).distinct(false);
    const auto cleared = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(cleared.sql().find(" ORDER BY ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find(" LIMIT ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find(" OFFSET ") == std::string_view::npos);
    RUVIA_CHECK(cleared.sql().find("DISTINCT") == std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { query.limit(static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1); }));
    RUVIA_CHECK(testing::throws_on([&] { query.offset(static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1); }));

    db_query derived;
    derived.select(derived.column("id")).from("cq_source");
    db_query source_options;
    source_options.select(source_options.star("d")).from(derived, "d", {.columns_ = {{.name_ = "id"}}});
    const auto source_statement = source_options.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(source_statement.sql().find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"d\" (\"id\")") != std::string_view::npos);
    const auto maria_source_statement = source_options.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(maria_source_statement.sql().find("FROM (SELECT `id` FROM `cq_source`) AS `d` (`id`)") != std::string_view::npos);

    db_query record_source;
    const auto record_function = record_source.call("jsonb_to_record", {record_source.value(R"({"id":1})")});
    record_source.select(record_source.star("r"))
        .from_function(record_function, "r",
            {.columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}},
                 {.name_ = "name", .type_ = {.data_type_ = db_data_type::varchar, .length_ = 8}}}});
    const auto record_statement = record_source.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(record_statement.sql().find("AS \"r\" (\"id\" INTEGER, \"name\" VARCHAR(8))") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)record_source.compile(db_driver::mariadb, nullptr); }));

    db_query lateral;
    lateral.select(lateral.star("d")).from(derived, "d", {.lateral_ = true});
    RUVIA_CHECK(compile_sql([&] { return lateral.compile(db_driver::postgresql, nullptr); }).find("FROM LATERAL") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)lateral.compile(db_driver::mariadb, nullptr); }));
    db_query bad_ordinality;
    bad_ordinality.select(bad_ordinality.star("d")).from(derived, "d", {.with_ordinality_ = true});
    RUVIA_CHECK(testing::throws_on([&] { (void)bad_ordinality.compile(db_driver::postgresql, nullptr); }));

    const std::array joins{
        std::pair{db_join_type::inner, std::string_view(" INNER JOIN ")},
        std::pair{db_join_type::left, std::string_view(" LEFT JOIN ")},
        std::pair{db_join_type::right, std::string_view(" RIGHT JOIN ")},
        std::pair{db_join_type::full, std::string_view(" FULL JOIN ")},
    };
    for (const auto& [type, token] : joins) {
        db_query joined;
        joined.select(joined.star("a")).from("cq_a", "a");
        const auto on = joined.binary(joined.column("id", "a"), op_type::equal, joined.column("id", "b"));
        joined.join(type, "cq_b", on, "b");
        const auto postgres = joined.compile(db_driver::postgresql, nullptr);
        RUVIA_CHECK(postgres.sql().find(token) != std::string_view::npos);
        if (type == db_join_type::full) {
            RUVIA_CHECK(testing::throws_on([&] { (void)joined.compile(db_driver::mariadb, nullptr); }));
        } else {
            const auto maria = joined.compile(db_driver::mariadb, nullptr);
            RUVIA_CHECK(maria.sql().find(token) != std::string_view::npos);
        }
    }
    db_query cross;
    cross.select(cross.star("a")).from("cq_a", "a").join(db_join_type::cross, "cq_b", {}, "b");
    RUVIA_CHECK(compile_sql([&] { return cross.compile(db_driver::postgresql, nullptr); }).find(" CROSS JOIN ") != std::string_view::npos);
    cross.join(db_join_type::cross, "cq_c", cross.column("id"), "c");
    RUVIA_CHECK(testing::throws_on([&] { (void)cross.compile(db_driver::postgresql, nullptr); }));
}

RUVIA_TEST(db_query_dml_and_conflict_overloads_cover_postgresql_and_mariadb) {
    db_query insert;
    const std::array<std::string_view, 2> columns{"id", "name"};
    insert.insert_into("cq_devices", std::span<const std::string_view>(columns), "d");
    const std::array row{insert.value(1), insert.value("first")};
    insert.values(std::span<const db_query::expr_type>(row));
    const auto inserted = insert.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(inserted.sql().find("INSERT INTO \"cq_devices\" AS \"d\"") != std::string_view::npos);
    RUVIA_CHECK(!inserted.returns_rows());

    db_query defaults;
    defaults.insert_into("cq_devices");
    const auto default_pg = defaults.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(default_pg.sql().find("DEFAULT VALUES") != std::string_view::npos);
    RUVIA_CHECK(compile_sql([&] { return defaults.compile(db_driver::mariadb, nullptr); }).find("() VALUES ()") != std::string_view::npos);

    db_query source;
    source.select({source.column("id"), source.column("name")}).from("cq_source");
    db_query insert_from;
    insert_from.insert_into("cq_devices", {"id", "name"}).insert_from(source);
    RUVIA_CHECK(compile_sql([&] { return insert_from.compile(db_driver::postgresql, nullptr); }).find("SELECT \"id\", \"name\"") != std::string_view::npos);

    db_query do_nothing;
    do_nothing.insert_into("cq_devices", {"id", "name"}).values({do_nothing.value(1), do_nothing.value("x")});
    do_nothing.on_conflict({.columns_ = {"id"}, .do_nothing_ = true});
    RUVIA_CHECK(compile_sql([&] { return do_nothing.compile(db_driver::postgresql, nullptr); }).find("ON CONFLICT (\"id\") DO NOTHING") != std::string_view::npos);

    db_query constraint;
    constraint.insert_into("cq_devices", {"id", "name"}).values({constraint.value(1), constraint.value("x")});
    constraint.on_conflict({.constraint_ = "cq_devices_pkey", .do_nothing_ = true});
    RUVIA_CHECK(compile_sql([&] { return constraint.compile(db_driver::postgresql, nullptr); }).find("ON CONSTRAINT \"cq_devices_pkey\" DO NOTHING") != std::string_view::npos);

    db_query targeted;
    targeted.insert_into("cq_devices", {"id", "name"}).values({targeted.value(1), targeted.value("x")});
    const auto active = targeted.binary(targeted.column("active"), op_type::equal, targeted.value(true));
    targeted.on_conflict({.columns_ = {"id"}, .target_where_ = active, .update_ = {{"name", targeted.excluded("name")}}, .update_where_ = targeted.binary(targeted.column("name"), op_type::not_equal, targeted.excluded("name"))});
    const auto targeted_statement = targeted.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(targeted_statement.sql().find("ON CONFLICT (\"id\") WHERE (\"active\" = $3) DO UPDATE SET") != std::string_view::npos);
    RUVIA_CHECK(targeted_statement.sql().find("WHERE (\"name\" <> ") != std::string_view::npos);
    RUVIA_CHECK(targeted_statement.sql().find("excluded.\"name\")") != std::string_view::npos ||
                targeted_statement.sql().find("\"excluded\".\"name\")") != std::string_view::npos);

    db_query maria_upsert;
    maria_upsert.insert_into("cq_devices", {"id", "name"}).values({maria_upsert.value(1), maria_upsert.value("x")});
    maria_upsert.on_conflict({.update_ = {{"name", maria_upsert.excluded("name")}}, .any_unique_key_ = true});
    const auto maria_statement = maria_upsert.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(maria_statement.sql().find("ON DUPLICATE KEY UPDATE \"name\"") == std::string_view::npos);
    RUVIA_CHECK(maria_statement.sql().find("ON DUPLICATE KEY UPDATE `name` = VALUES(`name`)") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)maria_upsert.compile(db_driver::postgresql, nullptr); }));
    db_query invalid_conflict;
    invalid_conflict.insert_into("cq_devices", {"id"}).values({invalid_conflict.value(1)});
    invalid_conflict.on_conflict({.columns_ = {"id"}, .constraint_ = "other", .do_nothing_ = true});
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid_conflict.compile(db_driver::postgresql, nullptr); }));

    db_query update;
    update.update("cq_devices", "d").set("name", update.value("updated"));
    update.set("name", update.value("updated-again"));
    db_query update_from;
    update_from.select(update_from.column("id")).from("cq_source");
    update.update_from(update_from, "s").where(update.binary(update.column("id", "d"), op_type::equal, update.column("id", "s")));
    RUVIA_CHECK(compile_sql([&] { return update.compile(db_driver::postgresql, nullptr); }).find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);

    db_query remove;
    remove.delete_from("cq_devices", "d");
    db_query delete_source;
    delete_source.select(delete_source.column("id")).from("cq_source");
    remove.delete_using(delete_source, "s").where(remove.binary(remove.column("id", "d"), op_type::equal, remove.column("id", "s")));
    RUVIA_CHECK(compile_sql([&] { return remove.compile(db_driver::postgresql, nullptr); }).find("USING (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)remove.compile(db_driver::mariadb, nullptr); }));

    db_query returning;
    returning.insert_into("cq_devices", {"id"}).values({returning.value(1)}).returning({returning.column("id")});
    const auto returning_statement = returning.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(returning_statement.returns_rows());
    RUVIA_CHECK(testing::throws_on([&] { (void)returning.compile(db_driver::mariadb, nullptr); }));
}

RUVIA_TEST(db_query_lock_cte_and_set_operation_enumerations_cover_supported_and_rejected_paths) {
    struct lock_case final {
        db_row_lock mode_;
        std::string_view postgres_token_;
        std::string_view maria_token_;
    };
    const std::array locks{
        lock_case{db_row_lock::update, "FOR UPDATE", "FOR UPDATE"},
        lock_case{db_row_lock::no_key_update, "FOR NO KEY UPDATE", {}},
        lock_case{db_row_lock::share, "FOR SHARE", "LOCK IN SHARE MODE"},
        lock_case{db_row_lock::key_share, "FOR KEY SHARE", {}},
    };
    for (const auto& item : locks) {
        db_query query;
        query.select(query.star("d")).from("cq_devices", "d").lock({.mode_ = item.mode_});
        const auto postgres = query.compile(db_driver::postgresql, nullptr);
        RUVIA_CHECK(postgres.sql().find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_token_.empty()) {
            RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));
        } else {
            RUVIA_CHECK(compile_sql([&] { return query.compile(db_driver::mariadb, nullptr); }).find(item.maria_token_) != std::string_view::npos);
        }
    }
    db_query scoped_lock;
    scoped_lock.select(scoped_lock.star("d")).from("cq_devices", "d").lock({.mode_ = db_row_lock::update, .nowait_ = true, .tables_ = {"d"}});
    RUVIA_CHECK(compile_sql([&] { return scoped_lock.compile(db_driver::postgresql, nullptr); }).find("FOR UPDATE OF \"d\" NOWAIT") != std::string_view::npos);
    db_query skip_locked;
    skip_locked.select(skip_locked.star()).from("cq_devices").lock({.skip_locked_ = true});
    RUVIA_CHECK(compile_sql([&] { return skip_locked.compile(db_driver::postgresql, nullptr); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    RUVIA_CHECK(compile_sql([&] { return skip_locked.compile(db_driver::mariadb, nullptr); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    db_query cleared;
    cleared.select(cleared.star()).from("cq_devices").lock({}).clear_lock();
    RUVIA_CHECK(compile_sql([&] { return cleared.compile(db_driver::postgresql, nullptr); }).find(" FOR ") == std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { cleared.lock({.nowait_ = true, .skip_locked_ = true}); }));
    db_query shared_skip;
    shared_skip.select(shared_skip.star()).from("cq_devices").lock({.mode_ = db_row_lock::share, .skip_locked_ = true});
    RUVIA_CHECK(testing::throws_on([&] { (void)shared_skip.compile(db_driver::mariadb, nullptr); }));

    db_query cte_body;
    cte_body.select(cte_body.value(1)).from("cq_seed");
    db_query recursive;
    recursive.with("numbers", cte_body, {.recursive_ = true, .materialization_ = db_materialization::not_materialized, .columns_ = {"n"}})
        .select(recursive.column("n"))
        .from("numbers");
    const auto recursive_statement = recursive.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(recursive_statement.sql().find("WITH RECURSIVE \"numbers\" (\"n\") AS NOT MATERIALIZED") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)recursive.compile(db_driver::mariadb, nullptr); }));
    db_query duplicate_cte;
    duplicate_cte.with("one", cte_body);
    RUVIA_CHECK(testing::throws_on([&] { duplicate_cte.with("one", cte_body); }));

    struct set_case final {
        db_set_operation operation_;
        std::string_view postgres_token_;
        bool maria_supported_;
    };
    const std::array set_cases{
        set_case{db_set_operation::union_value, " UNION ", true},
        set_case{db_set_operation::union_all, " UNION ALL ", true},
        set_case{db_set_operation::intersect, " INTERSECT ", true},
        set_case{db_set_operation::intersect_all, " INTERSECT ALL ", false},
        set_case{db_set_operation::except, " EXCEPT ", true},
        set_case{db_set_operation::except_all, " EXCEPT ALL ", false},
    };
    for (const auto& item : set_cases) {
        db_query first;
        first.select(first.value(1));
        db_query second;
        second.select(second.value(2));
        first.combine(item.operation_, second);
        RUVIA_CHECK(compile_sql([&] { return first.compile(db_driver::postgresql, nullptr); }).find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_supported_) {
            RUVIA_CHECK(compile_sql([&] { return first.compile(db_driver::mariadb, nullptr); }).find(item.postgres_token_) != std::string_view::npos);
        } else {
            RUVIA_CHECK(testing::throws_on([&] { (void)first.compile(db_driver::mariadb, nullptr); }));
        }
    }
}

RUVIA_TEST(db_query_window_frame_and_date_part_enumerations_cover_each_value) {
    const std::array frames{db_window_frame::rows, db_window_frame::range, db_window_frame::groups};
    for (const auto frame : frames) {
        db_query query;
        query.select(query.over(query.call("row_number"),
                         {.frame_ = db_window_frame_options{.kind_ = frame,
                              .start_ = {db_frame_boundary::unbounded_preceding, 0},
                              .end_ = {db_frame_boundary::current_row, 0}}}))
            .from("cq_window");
        const auto postgres = query.compile(db_driver::postgresql, nullptr);
        const auto token = frame == db_window_frame::rows ? "ROWS" : frame == db_window_frame::range ? "RANGE"
                                                                                                     : "GROUPS";
        RUVIA_CHECK(postgres.sql().find(token) != std::string_view::npos);
        if (frame == db_window_frame::groups) {
            RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));
        } else {
            RUVIA_CHECK(compile_sql([&] { return query.compile(db_driver::mariadb, nullptr); }).find(token) != std::string_view::npos);
        }
    }

    const std::array boundaries{
        std::pair{db_frame_boundary::unbounded_preceding, std::string_view("UNBOUNDED PRECEDING")},
        std::pair{db_frame_boundary::preceding, std::string_view("2 PRECEDING")},
        std::pair{db_frame_boundary::current_row, std::string_view("CURRENT ROW")},
        std::pair{db_frame_boundary::following, std::string_view("3 FOLLOWING")},
        std::pair{db_frame_boundary::unbounded_following, std::string_view("UNBOUNDED FOLLOWING")},
    };
    for (const auto& [boundary, token] : boundaries) {
        db_query query;
        const auto end = boundary == db_frame_boundary::unbounded_following ? boundary : db_frame_boundary::unbounded_following;
        const auto start = boundary == db_frame_boundary::unbounded_following ? db_frame_boundary::current_row : boundary;
        const auto start_value = start == db_frame_boundary::preceding ? 2U : start == db_frame_boundary::following ? 3U
                                                                                                                    : 0U;
        const auto end_value = end == db_frame_boundary::preceding ? 2U : end == db_frame_boundary::following ? 3U
                                                                                                              : 0U;
        query.select(query.over(query.call("row_number"),
                         {.frame_ = db_window_frame_options{.kind_ = db_window_frame::rows,
                              .start_ = {start, start_value},
                              .end_ = {end, end_value}}}))
            .from("cq_window");
        RUVIA_CHECK(compile_sql([&] { return query.compile(db_driver::postgresql, nullptr); }).find(token) != std::string_view::npos);
    }
    db_query invalid_boundary;
    invalid_boundary.select(invalid_boundary.over(invalid_boundary.call("row_number"),
                                {.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::current_row, 1}}}))
        .from("cq_window");
    RUVIA_CHECK(testing::throws_on([&] { (void)invalid_boundary.compile(db_driver::postgresql, nullptr); }));
    db_query reversed;
    reversed.select(reversed.over(reversed.call("row_number"),
                        {.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::following, 0},
                             .end_ = {db_frame_boundary::preceding, 0}}}))
        .from("cq_window");
    RUVIA_CHECK(testing::throws_on([&] { (void)reversed.compile(db_driver::postgresql, nullptr); }));

    struct date_case final {
        db_date_part part_;
        std::string_view token_;
        bool maria_supported_;
    };
    const std::array dates{
        date_case{db_date_part::epoch, "EPOCH", false},
        date_case{db_date_part::year, "YEAR", true},
        date_case{db_date_part::month, "MONTH", true},
        date_case{db_date_part::day, "DAY", true},
        date_case{db_date_part::hour, "HOUR", true},
        date_case{db_date_part::minute, "MINUTE", true},
        date_case{db_date_part::second, "SECOND", true},
        date_case{db_date_part::dow, "DOW", false},
        date_case{db_date_part::doy, "DOY", false},
        date_case{db_date_part::week, "WEEK", true},
        date_case{db_date_part::quarter, "QUARTER", true},
    };
    for (const auto& item : dates) {
        db_query query;
        query.select(query.extract(item.part_, query.column("created_at"))).from("cq_window");
        RUVIA_CHECK(compile_sql([&] { return query.compile(db_driver::postgresql, nullptr); }).find(item.token_) != std::string_view::npos);
        if (item.maria_supported_) {
            RUVIA_CHECK(compile_sql([&] { return query.compile(db_driver::mariadb, nullptr); }).find(item.token_) != std::string_view::npos);
        } else {
            RUVIA_CHECK(testing::throws_on([&] { (void)query.compile(db_driver::mariadb, nullptr); }));
        }
    }
}

RUVIA_TEST(db_query_expression_ownership_import_subquery_cast_and_cache_methods_are_observable) {
    db_query source;
    const auto qualified = source.column("id", "source");
    db_query target;
    const auto remapped = target.import_expression(qualified, "source", "target");
    target.select(remapped).from("cq_devices", "target");
    RUVIA_CHECK_EQ(compile_sql([&] { return target.compile(db_driver::postgresql, nullptr); }), "SELECT \"target\".\"id\" FROM \"cq_devices\" AS \"target\"");
    RUVIA_CHECK(testing::throws_on([&] { (void)target.import_expression(qualified, "source", {}); }));
    RUVIA_CHECK(testing::throws_on([&] { (void)target.import_expression({}, "source", "target"); }));

    db_query child;
    child.select(child.value(1)).from("cq_devices");
    db_query nested;
    const auto exists = nested.exists(child);
    const auto scalar = nested.subquery(child);
    nested.select({exists, scalar}).from("cq_devices");
    const auto nested_statement = nested.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(nested_statement.sql().find("EXISTS (SELECT $1 FROM \"cq_devices\")") != std::string_view::npos);
    RUVIA_CHECK(nested_statement.sql().find("(SELECT $2 FROM \"cq_devices\")") != std::string_view::npos);
    db_query dml;
    dml.delete_from("cq_devices").returning({dml.column("id")});
    RUVIA_CHECK(testing::throws_on([&] { (void)nested.subquery(dml); }));

    db_query typed;
    const auto number = typed.cast(typed.value(1), db_data_type::integer);
    const auto sized = typed.cast(typed.value("x"), db_type_definition{.data_type_ = db_data_type::varchar, .length_ = 8});
    typed.select({number, sized}).from("cq_devices");
    const auto typed_pg = typed.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(typed_pg.sql().find("CAST($1 AS INTEGER)") != std::string_view::npos);
    RUVIA_CHECK(typed_pg.sql().find("CAST($2 AS VARCHAR(8))") != std::string_view::npos);
    db_query named_type;
    named_type.select(named_type.cast(named_type.value(1), db_type_definition{.custom_name_ = "cq_custom_type"})).from("cq_devices");
    RUVIA_CHECK(compile_sql([&] { return named_type.compile(db_driver::postgresql, nullptr); }).find("CAST($1 AS \"cq_custom_type\")") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)named_type.compile(db_driver::mariadb, nullptr); }));

    db_query named;
    const std::array<db_named_argument, 1> named_arguments{{db_named_argument{"precision", named.value(2)}}};
    named.select(named.call("round", {}, named_arguments)).from("cq_devices");
    RUVIA_CHECK(compile_sql([&] { return named.compile(db_driver::postgresql, nullptr); }).find("\"precision\" => $1") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)named.compile(db_driver::mariadb, nullptr); }));
    RUVIA_CHECK(testing::throws_on([&] {
        const std::array<db_named_argument, 2> duplicate{{db_named_argument{"x", named.value(1)}, db_named_argument{"x", named.value(2)}}};
        (void)named.call("round", {}, duplicate);
    }));

    db_query state;
    RUVIA_CHECK(state.resource() != nullptr);
    RUVIA_CHECK(state.returns_rows());
    RUVIA_CHECK(!state.has_where());
    RUVIA_CHECK(!state.has_grouping());
    state.cache(false).cache(std::chrono::milliseconds(50)).cache(db_cache_options{.id_ = "cq-cache", .milliseconds_ = std::chrono::milliseconds(25)});
    state.select(state.value(1)).from("cq_devices");
    const auto state_statement = state.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(state_statement.params().size() == std::size_t{1});
    RUVIA_CHECK(testing::throws_on([&] { state.cache("bad", std::chrono::milliseconds(0)); }));
    RUVIA_CHECK(testing::throws_on([&] { state.cache("bad", std::chrono::milliseconds(-1)); }));

    std::pmr::unsynchronized_pool_resource cloned_resource;
    auto clone = state.clone(&cloned_resource);
    const auto clone_sql = compile_sql([&] { return clone.compile(db_driver::postgresql, nullptr); });
    const auto state_sql = compile_sql([&] { return state.compile(db_driver::postgresql, nullptr); });
    RUVIA_CHECK_EQ(clone_sql, state_sql);
    db_query moved = std::move(clone);
    RUVIA_CHECK(moved.resource() == &cloned_resource);
    RUVIA_CHECK(testing::throws_on([&] { (void)clone.resource(); }));
    db_query assigned;
    assigned = std::move(moved);
    RUVIA_CHECK(assigned.returns_rows());
    RUVIA_CHECK(testing::throws_on([&] { (void)moved.returns_rows(); }));

    db_query values;
    values.values({values.value(1), values.value(2)});
    RUVIA_CHECK(values.returns_rows());
    db_query dml_insert;
    dml_insert.insert_into("cq_devices", {"id"}).values({dml_insert.value(1)});
    RUVIA_CHECK(!dml_insert.returns_rows());
    dml_insert.returning({dml_insert.column("id")});
    RUVIA_CHECK(dml_insert.returns_rows());
}

RUVIA_TEST(db_query_char_cast_and_array_predicate_render_for_supported_dialects) {
    db_query casts;
    casts.select(casts.cast(casts.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 64})).from("char_records");
    const auto postgres = casts.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(postgres.sql().find("CAST($1 AS CHAR(64))") != std::string_view::npos);

    const auto maria = casts.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(maria.sql().find("CAST(? AS CHAR(64))") != std::string_view::npos);

    db_query long_maria_cast;
    long_maria_cast.select(long_maria_cast.cast(long_maria_cast.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 256})).from("char_records");
    const auto long_maria_statement = long_maria_cast.compile(db_driver::mariadb, nullptr);
    RUVIA_CHECK(long_maria_statement.sql().find("AS CHAR(256)") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] {
        db_query invalid;
        invalid.select(invalid.cast(invalid.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 10485761})).from("char_records");
        (void)invalid.compile(db_driver::postgresql, nullptr);
    }));

    using char_codes_type = db_entity<"char_codes",
        db_column<"values", std::pmr::vector<std::pmr::string>, db_column_options{.data_type_ = db_data_type::char_value, .length_ = 8}>>;
    db_query array;
    auto contains = char_codes_type::column<"values">().array_contains({"zone-a"});
    array.from("char_codes").where(contains.expression(array));
    const auto array_statement = array.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(array_statement.sql().find("AS CHAR(8)[]") != std::string_view::npos);
    RUVIA_CHECK(testing::throws_on([&] { (void)array.compile(db_driver::mariadb, nullptr); }));
}

RUVIA_TEST(db_query_span_overloads_cover_expression_and_statement_builders) {
    db_query query;
    const auto left = query.column("left");
    const auto right = query.column("right");
    const std::array<db_query::expr_type, 2> expressions{left, right};
    const auto call = query.call("coalesce", std::span<const db_query::expr_type>(expressions));
    const auto aggregate = query.aggregate("count", std::span<const db_query::expr_type>(expressions));
    const auto coalesced = query.coalesce(std::span<const db_query::expr_type>(expressions));
    const auto greatest = query.greatest(std::span<const db_query::expr_type>(expressions));
    const auto least = query.least(std::span<const db_query::expr_type>(expressions));
    const auto tuple = query.tuple(std::span<const db_query::expr_type>(expressions));
    const auto list = query.list(std::span<const db_query::expr_type>(expressions));
    const auto array_value = query.array(std::span<const db_query::expr_type>(expressions));
    const std::array<db_case_branch, 1> branches{{{query.binary(left, db_binary_operator::equal, query.value(1)), right}}};
    const auto case_expression = query.case_when(std::span<const db_case_branch>(branches), query.value(0));
    const std::array<db_order_term, 1> order{{{left, db_order_direction::asc, db_nulls_order::default_value}}};
    const auto within = query.within_group(aggregate, std::span<const db_order_term>(order));
    query.select(std::span<const db_query::expr_type>(expressions)).add_select(call).from("cq_span").group_by(std::span<const db_query::expr_type>(&left, 1)).add_group_by(right).having(query.unary(db_unary_operator::is_not_null, coalesced)).and_having(query.binary(greatest, db_binary_operator::greater, least)).order_by(left).add_order_by(right);
    const auto statement = query.compile(db_driver::postgresql, nullptr);
    RUVIA_CHECK(statement.sql().find("GROUP BY") != std::string_view::npos);
    RUVIA_CHECK(statement.sql().find("HAVING") != std::string_view::npos);

    db_query distinct;
    const auto distinct_left = distinct.column("left");
    distinct.select(distinct_left);
    const std::array<db_query::expr_type, 1> distinct_expressions{distinct.column("left")};
    distinct.distinct_on(std::span<const db_query::expr_type>(distinct_expressions)).from("cq_span");
    RUVIA_CHECK(compile_sql([&] { return distinct.compile(db_driver::postgresql, nullptr); }).find("DISTINCT ON") != std::string_view::npos);

    db_query dml;
    const std::array<std::string_view, 1> columns{"value"};
    const std::array<db_query::expr_type, 1> row{dml.value(1)};
    dml.insert_into("cq_span", std::span<const std::string_view>(columns)).values(std::span<const db_query::expr_type>(row));
    const std::array<db_query::expr_type, 1> returned{dml.column("value")};
    dml.returning(std::span<const db_query::expr_type>(returned));
    RUVIA_CHECK(dml.compile(db_driver::postgresql, nullptr).returns_rows());

    (void)tuple;
    (void)list;
    (void)array_value;
    (void)case_expression;
    (void)within;
}

}  // namespace
