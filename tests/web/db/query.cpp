#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "db_entity.h"
#include "db_query.h"
#include "worker_task_fixture.h"

namespace {
using namespace ruvia;
namespace testing = ::ruvia::test;
using op_type = db_binary_operator;
RUVIA_DB_ENTITY(entity_type, "device", db_column<"id", std::int64_t>, db_column<"name", std::string>)

TEST(Ruvia, db_public_parameters_preserve_types_and_own_compiled_text) {
    const auto statement = [] {
        db_query query;
        const std::string text(128, 'x');
        query.select({db_expr::value(text), db_expr::value(-42), db_expr::value((std::numeric_limits<std::uint64_t>::max)()), db_expr::value(1.25), db_expr::value(true)});
        return query.compile(db_driver::postgresql);
    }();
    const db_value null_value(nullptr);
    EXPECT_EQ(null_value.type(), db_value_type::null);
    EXPECT_THROW((void)null_value.text(), std::bad_variant_access);
    const auto parameters = statement.params();
    ASSERT_EQ(parameters.size(), std::size_t{5});
    EXPECT_EQ(parameters[0].type(), db_value_type::string);
    EXPECT_EQ(parameters[0].text(), std::string(128, 'x'));
    EXPECT_EQ(parameters[1].type(), db_value_type::signed_value);
    EXPECT_EQ(parameters[1].signed_value(), -42);
    EXPECT_EQ(parameters[2].type(), db_value_type::unsigned_value);
    EXPECT_EQ(parameters[2].unsigned_value(), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(parameters[3].type(), db_value_type::double_);
    EXPECT_EQ(parameters[3].double_value(), 1.25);
    EXPECT_EQ(parameters[4].type(), db_value_type::bool_);
    EXPECT_TRUE(parameters[4].bool_value());
    EXPECT_THROW((void)parameters[0].signed_value(), std::bad_variant_access);
    EXPECT_THROW((void)parameters[1].unsigned_value(), std::bad_variant_access);
    EXPECT_THROW((void)parameters[2].double_value(), std::bad_variant_access);
    EXPECT_THROW((void)parameters[3].bool_value(), std::bad_variant_access);
}

TEST(Ruvia, db_query_sql_expression_import_preserves_syntax_and_parameter_binding) {
    {
        db_query target;
        {
            const auto expression = db_expr::raw_sql({"jsonb_set(", ", '{label}', to_jsonb(", "::text))"}, {db_expr::column("payload"), db_expr::value("x'); DELETE FROM device; --")});
            target.update("device").set("payload", expression);
            target.returning({db_expr::column("id")});
        }
        auto statement = target.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), "UPDATE \"device\" SET \"payload\" = (jsonb_set(\"payload\", '{label}', to_jsonb($1::text))) RETURNING \"id\"");
        EXPECT_EQ(statement.params().size(), std::size_t{1});
        EXPECT_TRUE(testing::throws_on([&] {
            target.where(db_expr::raw_sql({"one"}, {db_expr::value(1)}));
        }));
        EXPECT_TRUE(testing::throws_on([&] {
            target.where(db_expr::raw_sql(std::string_view("x\0y", 3)));
        }));
    }
}

template <typename fn_type>
std::string compile_sql(fn_type&& fn) {
    auto statement = fn();
    return std::string(statement.sql());
}

TEST(Ruvia, db_query_batch_set_rejects_partial_updates_and_keeps_last_duplicate) {
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        db_query query;
        query.update("accounts").set("balance", db_expr::value(100));
        query.where(db_expr::binary(db_expr::column("id"), op_type::equal, db_expr::value(7)));
        const auto original = compile_sql([&] {
            return query.compile(driver, db_parameter_mode::literal);
        });
        bool rejected = false;
        try {
            query.set({{"balance", db_expr::value(0)}, {"", db_expr::value(1)}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(compile_sql([&] {
            return query.compile(driver, db_parameter_mode::literal);
        }),
            original);
        query.set({{"balance", db_expr::value(1)}, {"revision", db_expr::value(2)}, {"balance", db_expr::value(3)}});
        const auto committed = query.compile(driver, db_parameter_mode::literal);
        EXPECT_EQ(committed.sql(), driver == db_driver::postgresql ? "UPDATE \"accounts\" SET \"balance\" = 3, \"revision\" = 2 WHERE (\"id\" = 7)" : "UPDATE `accounts` SET `balance` = 3, `revision` = 2 WHERE (`id` = 7)");
    }
}

TEST(Ruvia, db_query_insert_target_cannot_reinterpret_rows_and_rejected_batches_preserve_them) {
    db_query query;
    query.insert_into("pairs", {"a"}).values({db_expr::value(11)});
    const auto original = compile_sql([&] {
        return query.compile(db_driver::postgresql, db_parameter_mode::literal);
    });
    EXPECT_TRUE(testing::throws_on([&] {
        query.insert_into("pairs", {"b"});
    }));
    const std::array<std::string_view, 1> columns{"b"};
    const std::array values{db_expr::value(22)};
    EXPECT_TRUE(testing::throws_on([&] {
        query.insert_rows(columns, values, 2);
    }));
    db_query source;
    source.select(db_expr::value(9));
    EXPECT_TRUE(testing::throws_on([&] {
        query.insert_from(source);
    }));
    EXPECT_EQ(compile_sql([&] {
        return query.compile(db_driver::postgresql, db_parameter_mode::literal);
    }),
        original);
    query.insert_rows(columns, values, 1);
    const auto committed = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(committed.sql(), "INSERT INTO \"pairs\" (\"a\", \"b\") VALUES (11, DEFAULT), (DEFAULT, 22)");
}

TEST(Ruvia, db_query_reuses_postgresql_parameters_for_grouped_expressions) {
    db_query query;
    const auto bucket = db_expr::binary(db_expr::column("id"), op_type::add, db_expr::value(1)).expression(query);
    query.select({db_expr(bucket), db_expr::aggregate("count", {db_expr::star()})}).from("events").group_by({bucket});
    const auto postgres = query.compile(db_driver::postgresql);
    EXPECT_EQ(postgres.sql(), "SELECT (\"id\" + $1), \"count\"(*) FROM \"events\" GROUP BY (\"id\" + $1)");
    EXPECT_EQ(postgres.params().size(), std::size_t{1});
    const auto maria = query.compile(db_driver::mariadb);
    EXPECT_EQ(maria.params().size(), std::size_t{2});
}

TEST(Ruvia, db_query_binds_values_in_wire_order_and_preserves_zero_limit) {
    db_query query;
    const auto second = db_expr::value(db_value("O'Reilly $1 ?"));
    const auto first = db_expr::value(db_value(9));
    const auto id = db_expr::column("id", "u");
    query.select(first).from("users", "u").where(db_expr::binary(id, op_type::equal, second));
    query.order_by(id, order_direction::desc).limit(0).lock({.nowait_ = true});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT $1 FROM \"users\" AS \"u\" WHERE (\"u\".\"id\" = $2) ORDER BY \"u\".\"id\" DESC LIMIT $3 FOR UPDATE NOWAIT");
    EXPECT_EQ(statement.params().size(), std::size_t{3});
}

TEST(Ruvia, db_query_nested_ctes_are_owned_and_use_one_parameter_sequence) {
    db_query query;
    {
        db_query incoming;
        const std::array row{db_expr::value(db_value(7)), db_expr::value(db_value("first"))};
        incoming.values(row);
        std::string column_name("name");
        const borrowed_text columns[]{"id", column_name};
        const db_cte_options options{.materialization_ = db_materialization::materialized, .columns_ = columns};
        query.with("incoming", incoming, options);
        column_name.assign("gone");
        incoming.values(row);
    }
    db_query filter;
    filter.select(db_expr::column("id")).from("incoming").where(db_expr::binary(db_expr::column("name"), op_type::equal, db_expr::value(db_value("first"))));
    query.select(db_expr::value(db_value(11))).from(filter, "filtered");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "WITH \"incoming\" (\"id\", \"name\") AS MATERIALIZED (VALUES ($1, $2)) SELECT $3 FROM (SELECT \"id\" FROM \"incoming\" WHERE (\"name\" = $4)) AS \"filtered\"");
    EXPECT_EQ(statement.params().size(), std::size_t{4});
}

TEST(Ruvia, db_query_appended_join_and_cte_metadata_remain_owned) {
    db_query query;
    query.select(db_expr::column("id")).from("base_rows");
    std::string expected_ctes = "WITH ";
    std::string expected_joins;
    for (int i = 0; i < 6; ++i) {
        std::string source_name = "long_query_source_" + std::to_string(i);
        std::string column_name = "long_identity_column_" + std::to_string(i);
        std::string alias = "long_join_source_alias_" + std::to_string(i);
        db_query source;
        source.select(db_expr::value(i));
        const borrowed_text columns[]{column_name};
        query.with(source_name, source, {.columns_ = columns});
        query.join(db_join_type::inner, source_name, {}, alias, {column_name});
        if (i != 0) {
            expected_ctes += ", ";
        }
        expected_ctes += "\"" + source_name + "\" (\"" + column_name + "\") AS (SELECT $" + std::to_string(i + 1) + ")";
        expected_joins += " INNER JOIN \"" + source_name + "\" AS \"" + alias + "\" USING (\"" + column_name + "\")";
        source_name.assign("gone");
        column_name.assign("gone");
        alias.assign("gone");
        source.select(db_expr::value(-1));
    }
    const auto expected = expected_ctes + " SELECT \"id\" FROM \"base_rows\"" + expected_joins;
    auto snapshot = query.clone();
    query.select(db_expr::value(-1));
    const auto statement = snapshot.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), expected);
    ASSERT_EQ(statement.params().size(), std::size_t{6});
    for (std::size_t i = 0; i < statement.params().size(); ++i) {
        EXPECT_EQ(statement.params()[i].signed_value(), static_cast<std::int64_t>(i));
    }
}

TEST(Ruvia, db_query_postgresql_casts_bound_values_only_where_nothing_types_them) {
    db_query query;
    const auto name = db_expr::column("name");
    const auto payload = db_expr::column("payload");
    const std::array<db_selection, 20> projections{
        // "any" arguments give an untyped parameter nothing to resolve against.
        db_expr::call("json_build_object", {db_expr::value("k"), db_expr::value(5), db_expr::value("on"), db_expr::value(true)}),
        db_expr::call("concat", {name, db_expr::value("-"), db_expr::value(1.5)}),
        // int4-only signatures are typed int4 explicitly.
        db_expr::call("left", {name, db_expr::value(3)}),
        db_expr::call("pg_catalog.round", {db_expr::column("price"), db_expr::value(2)}),
        // Other arguments keep the type of their context (uuid, json, timestamp...).
        db_expr::call("date_trunc", {db_expr::value("day"), db_expr::column("created_at")}),
        // Arithmetic over two bound operands selects no overload; one typed
        // operand types the other. unknown || unknown resolves to text.
        db_expr::binary(db_expr::value("a"), op_type::concat, db_expr::value("b")),
        db_expr::binary(name, op_type::concat, db_expr::value("b")),
        db_expr::binary(db_expr::column("created_on"), op_type::add, db_expr::value(1)),
        db_expr::binary(db_expr::value(2), op_type::multiply, db_expr::value(3)),
        // An integer jsonb subscript is an array index, never a text key.
        db_expr::binary(payload, op_type::json_get, db_expr::value(0)),
        db_expr::binary(payload, op_type::json_get_text, db_expr::value("key")),
        // A typed function overload resolves its untyped argument; negation
        // and GREATEST over bound values have nothing to resolve against.
        db_expr::call("abs", {db_expr::value(-3)}),
        db_expr::unary(db_unary_operator::negate, db_expr::value(4)),
        db_expr::greatest({db_expr::value(1), db_expr::value(2)}),
        // The timestamp overload types the stop and step arguments.
        db_expr::call("generate_series", {db_expr::column("start_at"), db_expr::value("2026-01-02 00:00:00"), db_expr::value("1 day")}),
        // A separator is text; the "any" arguments after it are not.
        db_expr::call("concat_ws", {db_expr::value(","), db_expr::value(1)}),
        // Boolean predicates and logical operators type their operands.
        db_expr::unary(db_unary_operator::is_true, db_expr::value("true")),
        db_expr::binary(db_expr::value(true), op_type::and_, db_expr::value(false)),
        // The null predicates accept every type, so nothing types the operand.
        db_expr::unary(db_unary_operator::is_null, db_expr::value(7)),
        // Comparing two bound values resolves as text.
        db_expr::binary(db_expr::value("x"), op_type::equal, db_expr::value("y")),
    };
    query.select(projections).from("items").where(db_expr::binary(db_expr::column("ratio"), op_type::greater, db_expr::value(0.25)));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(),
        "SELECT \"json_build_object\"($1::text, $2::int8, $3::text, $4::bool), \"concat\"(\"name\", $5::text, $6::float8), "
        "\"left\"(\"name\", $7::int4), \"pg_catalog\".\"round\"(\"price\", $8::int4), \"date_trunc\"($9, \"created_at\"), "
        "($10 || $11), (\"name\" || $12), (\"created_on\" + $13), ($14::int8 * $15::int8), "
        "(\"payload\" -> $16::int4), (\"payload\" ->> $17), \"abs\"($18), (- $19::int8), GREATEST($20::int8, $21::int8), "
        "\"generate_series\"(\"start_at\", $22, $23), \"concat_ws\"($24, $25::int8), ($26 IS TRUE), ($27 AND $28), "
        "($29::int8 IS NULL), ($30 = $31) "
        "FROM \"items\" WHERE (\"ratio\" > $32)");
    EXPECT_EQ(statement.params().size(), std::size_t{32});
    // An explicit cast names the type, so the placeholder stays untyped.
    db_query cast;
    cast.select(db_expr::value(7).cast(db_type_definition{.custom_name_ = "app_code"})).from("items");
    const auto cast_statement = cast.compile(db_driver::postgresql);
    EXPECT_EQ(cast_statement.sql(), "SELECT CAST($1 AS \"app_code\") FROM \"items\"");
    // Concatenation never forces a category (int8 || int8 does not exist).
    db_query reuse;
    const std::array<db_selection, 2> reuse_projections{db_expr::binary(db_expr::value(1), op_type::concat, db_expr::value(2)), db_expr::value("1.5").cast(db_type_definition{.data_type_ = db_data_type::numeric})};
    reuse.select(reuse_projections).from("items");
    const auto reuse_statement = reuse.compile(db_driver::postgresql);
    EXPECT_EQ(reuse_statement.sql(), "SELECT ($1 || $2), CAST($3 AS DECIMAL) FROM \"items\"");
    db_query maria_cast;
    maria_cast.select(db_expr::value("1.5").cast(db_type_definition{.data_type_ = db_data_type::numeric}));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)maria_cast.compile(db_driver::mariadb);
    }));
    db_query maria_time;
    maria_time.select(db_expr::value("2026-01-01 00:00:00.5").cast(db_type_definition{.data_type_ = db_data_type::timestamp}));
    const auto maria_time_statement = maria_time.compile(db_driver::mariadb);
    EXPECT_EQ(maria_time_statement.sql(), "SELECT CAST(? AS DATETIME(6))");
    // Unsigned values span beyond bigint, so their category is numeric:
    // COALESCE over UINT64_MAX must not narrow it to int8.
    db_query wide;
    wide.select(db_expr::coalesce({db_expr::value(std::numeric_limits<std::uint64_t>::max()), db_expr::value(std::uint64_t{1})})).from("items");
    const auto wide_statement = wide.compile(db_driver::postgresql);
    EXPECT_EQ(wide_statement.sql(), "SELECT COALESCE($1::numeric, $2::numeric) FROM \"items\"");
    EXPECT_EQ(wide_statement.params().size(), std::size_t{2});
    const auto wide_literal = wide.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(wide_literal.sql(), "SELECT COALESCE(18446744073709551615, 1) FROM \"items\"");
    EXPECT_TRUE(wide_literal.params().empty());
    // MariaDB placeholders carry no casts.
    db_query maria;
    maria.select(db_expr::call("left", {db_expr::column("name"), db_expr::value(3)})).from("items").where(db_expr::binary(db_expr::column("ratio"), op_type::greater, db_expr::value(0.25)));
    const auto maria_statement = maria.compile(db_driver::mariadb);
    EXPECT_EQ(maria_statement.sql(), "SELECT left(`name`, ?) FROM `items` WHERE (`ratio` > ?)");
}

TEST(Ruvia, db_query_literal_doubles_keep_the_bound_double_type) {
    // Bound doubles are float8/DOUBLE; a literal 2 would make "qty / 2" integer
    // division and a literal 2.5 is exact NUMERIC/DECIMAL in both dialects.
    db_query query;
    query.select({db_expr::binary(db_expr::column("qty"), op_type::divide, db_expr::value(2.0)), db_expr::value(2.5), db_expr::value(-0.5), db_expr::value(1e20)}).from("items");
    const auto postgres = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(postgres.sql(),
        "SELECT (\"qty\" / CAST('2' AS DOUBLE PRECISION)), CAST('2.5' AS DOUBLE PRECISION), "
        "CAST('-0.5' AS DOUBLE PRECISION), CAST('1e+20' AS DOUBLE PRECISION) FROM \"items\"");
    const auto maria = query.compile(db_driver::mariadb, db_parameter_mode::literal);
    EXPECT_EQ(maria.sql(), "SELECT (`qty` / 2e0), 2.5e0, -0.5e0, 1e+20 FROM `items`");
    EXPECT_EQ(db_query::render_expression(db_expr::unary(db_unary_operator::negate, db_expr::value(-2.0)), db_driver::mariadb), "(- -2e0)");
}

TEST(Ruvia, db_value_rejects_finite_floating_overflow_before_query_construction) {
    if constexpr (std::numeric_limits<long double>::max_exponent > std::numeric_limits<double>::max_exponent) {
        for (const auto input : {std::numeric_limits<long double>::max(), -std::numeric_limits<long double>::max()}) {
            bool rejected = false;
            try {
                (void)db_value(input);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        }
    }
    const auto maximum = std::numeric_limits<double>::max();
    db_query converted;
    converted.select({db_expr::value(db_value(1.5L)), db_expr::value(db_value(static_cast<long double>(maximum))), db_expr::value(db_value(-static_cast<long double>(maximum)))});
    db_query reference;
    reference.select({db_expr::value(1.5), db_expr::value(maximum), db_expr::value(-maximum)});
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        const auto actual = converted.compile(driver, db_parameter_mode::literal);
        const auto expected = reference.compile(driver, db_parameter_mode::literal);
        EXPECT_EQ(actual.sql(), expected.sql());
    }
}

TEST(Ruvia, db_query_postgresql_bitwise_parameters_use_integer_overloads) {
    struct integer_case final {
        db_value value_;
        std::string_view literal_;
    };
    const std::array values{
        integer_case{db_value(std::numeric_limits<std::int64_t>::min()), "-9223372036854775808"},
        integer_case{db_value(std::numeric_limits<std::int64_t>::max()), "9223372036854775807"},
        integer_case{db_value(std::uint64_t{0}), "0"},
        integer_case{db_value(std::uint64_t{1}), "1"},
        integer_case{db_value(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())), "9223372036854775807"},
    };
    for (const auto& item : values) {
        db_query query;
        const auto operand = db_expr::value(item.value_);
        const auto mask = db_expr::value(std::uint64_t{1});
        const auto inverted = db_expr::unary(db_unary_operator::bit_not, operand);
        query.select({db_expr::binary(operand, op_type::bit_and, mask), db_expr::binary(operand, op_type::bit_or, mask), db_expr::binary(operand, op_type::bit_xor, mask), inverted});
        const auto statement = query.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), "SELECT ($1::int8 & $2::int8), ($3::int8 | $4::int8), ($5::int8 # $6::int8), (~ $7::int8)");
        EXPECT_EQ(statement.params().size(), std::size_t{7});
        const auto literal = query.compile(db_driver::postgresql, db_parameter_mode::literal);
        const auto operand_literal = "(" + std::string(item.literal_) + ")::int8";
        const auto literal_sql = std::string("SELECT (") + operand_literal + std::string(" & (1)::int8), (") + operand_literal + std::string(" | (1)::int8), (") + operand_literal + std::string(" # (1)::int8), (~ ") + operand_literal + ")";
        EXPECT_EQ(literal.sql(), std::string_view(literal_sql));
        EXPECT_TRUE(literal.params().empty());
        const auto inverted_sql = std::string("(~ ") + operand_literal + ")";
        EXPECT_EQ(db_query::render_expression(inverted, db_driver::postgresql), std::string_view(inverted_sql));
    }
}

TEST(Ruvia, db_query_collate_validates_qualified_collation_names) {
    for (const std::string_view invalid : {"a..b", "a.", ".b"}) {
        db_query query;
        bool rejected = false;
        try {
            query.select(db_expr::column("name").collate(invalid));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const auto collated = db_expr::column("name").collate("pg_catalog.C");
    EXPECT_EQ(db_query::render_expression(collated, db_driver::postgresql), std::string_view(R"(("name" COLLATE "pg_catalog"."C"))"));
}

TEST(Ruvia, db_query_postgresql_bitwise_parameters_reject_unsigned_overflow_in_both_modes) {
    const std::array values{
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1,
        std::numeric_limits<std::uint64_t>::max(),
    };
    for (const auto value : values) {
        for (const auto mode : {db_parameter_mode::bound, db_parameter_mode::literal}) {
            for (const auto op : {op_type::bit_and, op_type::bit_or, op_type::bit_xor}) {
                for (const bool overflow_on_left : {true, false}) {
                    db_query query;
                    const auto overflow = db_expr::value(value);
                    const auto mask = db_expr::value(std::uint64_t{1});
                    query.select(db_expr::binary(overflow_on_left ? overflow : mask, op, overflow_on_left ? mask : overflow));
                    bool rejected = false;
                    try {
                        (void)query.compile(db_driver::postgresql, mode);
                    } catch (const std::invalid_argument&) {
                        rejected = true;
                    }
                    EXPECT_TRUE(rejected);
                }
            }
            db_query query;
            query.select(db_expr::unary(db_unary_operator::bit_not, db_expr::value(value)));
            bool rejected = false;
            try {
                (void)query.compile(db_driver::postgresql, mode);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
            db_query reused;
            const auto overflow = db_expr::value(value);
            reused.select({db_expr::coalesce({overflow, overflow}), db_expr::unary(db_unary_operator::bit_not, overflow)});
            bool reused_rejected = false;
            try {
                (void)reused.compile(db_driver::postgresql, mode);
            } catch (const std::invalid_argument&) {
                reused_rejected = true;
            }
            EXPECT_TRUE(reused_rejected);
        }
    }
}

TEST(Ruvia, db_query_postgresql_bitwise_values_keep_column_and_explicit_cast_contexts) {
    db_query query;
    const auto value = db_expr::value(std::numeric_limits<std::uint64_t>::max());
    const auto flags = db_expr::column("flags");
    const auto cast = value.cast(db_type_definition{.custom_name_ = "app_flags"});
    query.select({db_expr::binary(flags, op_type::bit_and, value), db_expr::binary(value, op_type::bit_or, flags), db_expr::binary(cast, op_type::bit_xor, value), db_expr::unary(db_unary_operator::bit_not, cast)}).from("items");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT (\"flags\" & $1), ($2 | \"flags\"), (CAST($3 AS \"app_flags\") # $4), (~ CAST($5 AS \"app_flags\")) FROM \"items\"");
    EXPECT_EQ(statement.params().size(), std::size_t{5});
    const auto literal = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(literal.sql(),
        "SELECT (\"flags\" & 18446744073709551615), (18446744073709551615 | \"flags\"), "
        "(CAST(18446744073709551615 AS \"app_flags\") # 18446744073709551615), (~ CAST(18446744073709551615 AS \"app_flags\")) FROM \"items\"");
    EXPECT_TRUE(literal.params().empty());
}

TEST(Ruvia, db_value_borrows_string_lvalues) {
    std::string name("alice");
    const std::string fixed("bob");
    const ruvia::db_value from_mutable(name);
    const ruvia::db_value from_const(fixed);
    db_query query;
    query.select(db_expr::value(from_mutable)).from("items").where(db_expr::binary(db_expr::column("name"), op_type::equal, db_expr::value(from_const)));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.params().size(), std::size_t{2});
}

TEST(Ruvia, db_query_null_comparisons_and_empty_membership_have_defined_semantics) {
    db_query query;
    const auto id = db_expr::column("id");
    const std::array<db_selection, 4> projections{db_expr::binary(id, op_type::equal, db_expr::null()), db_expr::binary(db_expr::null(), op_type::not_equal, id), db_expr::binary(id, op_type::in, db_expr::list({})), db_expr::binary(id, op_type::not_in, db_expr::list({}))};
    query.select(projections).from("devices");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT (\"id\" IS NULL), (\"id\" IS NOT NULL), FALSE, TRUE FROM \"devices\"");
    EXPECT_TRUE(statement.params().empty());
}

TEST(Ruvia, db_query_null_comparisons_share_unary_parameter_typing) {
    const auto value = db_expr::value(7);
    const auto null = db_expr::null();
    const std::array predicates{value == nullptr, null == value, value != nullptr, null != value, value.is_null(), value.is_not_null()};
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        for (const auto mode : {db_parameter_mode::bound, db_parameter_mode::literal}) {
            for (std::size_t i = 0; i < predicates.size(); ++i) {
                const bool negate = i == 2 || i == 3 || i == 5;
                const auto op = negate ? db_binary_operator::not_equal : db_binary_operator::equal;
                db_query expected;
                expected.select(db_expr::unary(negate ? db_unary_operator::is_not_null : db_unary_operator::is_null, db_expr::value(7)));
                const auto reference = expected.compile(driver, mode);
                db_query owned;
                owned.select(db_expr(predicates[i]));
                const auto owned_statement = owned.compile(driver, mode);
                EXPECT_EQ(owned_statement.sql(), reference.sql());
                EXPECT_EQ(owned_statement.params().size(), reference.params().size());
                db_query direct;
                const auto operand = db_expr::value(7);
                const auto null_value = db_expr::null();
                direct.select(i == 1 || i == 3 ? db_expr::binary(null_value, op, operand) : db_expr::binary(operand, op, null_value));
                const auto direct_statement = direct.compile(driver, mode);
                EXPECT_EQ(direct_statement.sql(), reference.sql());
                EXPECT_EQ(direct_statement.params().size(), reference.params().size());
                if (driver == db_driver::postgresql && mode == db_parameter_mode::bound) {
                    EXPECT_TRUE(reference.sql().find("$1::int8 IS ") != std::string_view::npos);
                }
            }
        }
    }
}

TEST(Ruvia, db_query_window_aggregate_filter_and_lateral_source) {
    db_query query;
    const auto ts = db_expr::column("ts", "e");
    const auto id = db_expr::column("id", "e");
    const db_expr partitions[]{id};
    const db_order_term window_order[]{{ts, order_direction::desc}};
    const auto rank = db_expr::call("row_number").over({.partition_by_ = partitions, .order_by_ = window_order});
    const std::array args{id};
    const std::array order{db_order_term{ts, order_direction::asc}};
    const auto aggregate = db_expr::aggregate("array_agg", args, false, order).filter(id.is_not_null());
    const std::array<db_selection, 2> projections{rank.as("rank"), aggregate};
    query.select(projections).from("events", "e");
    query.join_function(db_join_type::cross, db_expr::call("jsonb_each", {db_expr::column("data", "e")}), {}, "kv", {.lateral_ = true});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_TRUE(statement.sql().find("\"row_number\"() OVER (PARTITION BY \"e\".\"id\" ORDER BY \"e\".\"ts\" DESC)") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("\"array_agg\"(\"e\".\"id\" ORDER BY \"e\".\"ts\" ASC) FILTER (WHERE (\"e\".\"id\" IS NOT NULL))") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("CROSS JOIN LATERAL \"jsonb_each\"(\"e\".\"data\") AS \"kv\"") != std::string_view::npos);
}

TEST(Ruvia, db_query_expression_helpers_render_postgresql_and_reject_empty_inputs) {
    db_query query;
    const auto left = db_expr::column("left");
    const auto right = db_expr::column("right");
    const auto values = db_expr::array({db_expr::value(1), db_expr::value(2)});
    const std::array ordered_by{db_order_term{db_expr::column("score"), order_direction::desc}};
    const auto ordered = db_expr::aggregate("percentile_cont", {db_expr::value(0.5)}).within_group(ordered_by);
    const auto branches = std::array{db_case_branch{db_expr::binary(left, op_type::greater, db_expr::value(0)), db_expr::value("positive")}};
    const std::array<db_selection, 12> projections{
        db_expr::coalesce({db_expr::null(), left}),
        db_expr::null_if(left, right),
        db_expr::greatest({left, right}),
        db_expr::least({left, right}),
        db_expr(db_tuple{left, right}),
        db_expr::any(values),
        db_expr::all(values),
        db_expr::case_when(branches, db_expr::value("zero")),
        ordered,
        db_expr::column("created_at").extract(db_date_part::year),
        db_expr::column("tags").subscript(db_expr::value(1)),
        db_expr::column("name").collate("C"),
    };
    query.select(projections).from("metrics");

    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT COALESCE(NULL, \"left\"), NULLIF(\"left\", \"right\"), GREATEST(\"left\", \"right\"), LEAST(\"left\", \"right\"), ROW(\"left\", \"right\"), ANY(ARRAY[$1, $2]), ALL(ARRAY[$3, $4]), CASE WHEN (\"left\" > $5) THEN $6 ELSE $7 END, \"percentile_cont\"($8) WITHIN GROUP (ORDER BY \"score\" DESC), EXTRACT(YEAR FROM \"created_at\"), (\"tags\")[$9], (\"name\" COLLATE \"C\") FROM \"metrics\"");
    EXPECT_EQ(statement.params().size(), std::size_t{9});

    db_query maria_array;
    maria_array.select(db_expr::any(db_expr::array({db_expr::value(1)}))).from("metrics");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)maria_array.compile(db_driver::mariadb);
    }));

    db_query maria_within_group;
    const auto aggregate = db_expr::aggregate("percentile_cont", {db_expr::value(0.5)});
    const std::array maria_order{db_order_term{db_expr::column("score"), order_direction::asc}};
    maria_within_group.select(aggregate.within_group(maria_order)).from("metrics");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)maria_within_group.compile(db_driver::mariadb);
    }));

    EXPECT_TRUE(testing::throws_on([] {
        (void)db_query::render_expression(db_expr::coalesce({}), db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([] {
        (void)db_query::render_expression(db_expr::greatest({}), db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([] {
        (void)db_query::render_expression(db_expr::least({}), db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_query::render_expression(left.within_group({}), db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([] {
        db_query empty_tuple;
        empty_tuple.select(db_expr(db_tuple{})).from("metrics");
        (void)empty_tuple.compile(db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_native_functions_and_identifier_quoting_are_dialect_safe) {
    db_query query;
    const auto amount = db_expr::column("amount", "s");
    const auto name = db_expr::column("name", "s");
    const auto count = db_expr::aggregate("count", {db_expr::star()});
    const auto sum = db_expr::aggregate("sum", {amount});
    const db_order_term window_order[]{{db_expr::column("id", "s"), order_direction::asc}};
    const auto row_number = db_expr::call("row_number").over({.order_by_ = window_order});
    const auto scalar = db_expr::call("lower", {name});
    query.select({count, sum, row_number, scalar}).from("sales", "s");

    const auto postgres = query.compile(db_driver::postgresql);
    EXPECT_EQ(postgres.sql(), "SELECT \"count\"(*), \"sum\"(\"s\".\"amount\"), \"row_number\"() OVER (ORDER BY \"s\".\"id\" ASC), \"lower\"(\"s\".\"name\") FROM \"sales\" AS \"s\"");

    const auto maria = query.compile(db_driver::mariadb);
    EXPECT_TRUE(maria.sql().find("count(*)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("sum(`s`.`amount`)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("row_number() OVER (ORDER BY `s`.`id` ASC)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("lower(`s`.`name`)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("`count`") == std::string_view::npos);

    db_query identifiers;
    const auto dangerous_column = db_expr::column("name\"; DROP TABLE users --", "tenant.schema");
    const auto dangerous_function = db_expr::call("pkg.weird\"function", {dangerous_column});
    const auto special_function = db_expr::call("weird`fn--", {dangerous_column});
    identifiers.select({dangerous_function, special_function}).from("tenant.schema", "s");
    const auto safe_postgres = identifiers.compile(db_driver::postgresql);
    EXPECT_TRUE(safe_postgres.sql().find("\"tenant\".\"schema\".\"name\"\"; DROP TABLE users --\"") != std::string_view::npos);
    EXPECT_TRUE(safe_postgres.sql().find("\"pkg\".\"weird\"\"function\"") != std::string_view::npos);
    const auto safe_maria = identifiers.compile(db_driver::mariadb);
    EXPECT_TRUE(safe_maria.sql().find("`tenant`.`schema`.`name\"; DROP TABLE users --`") != std::string_view::npos);
    EXPECT_TRUE(safe_maria.sql().find("`pkg`.`weird\"function`") != std::string_view::npos);
    EXPECT_TRUE(safe_maria.sql().find("`weird``fn--`") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([] {
        db_query invalid;
        invalid.select(db_expr::column(std::string_view("bad\0name", 8)));
    }));

    const std::string overlong(65, 'x');
    db_query overlong_query;
    overlong_query.select(db_expr::call(overlong)).from("events");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)overlong_query.compile(db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)overlong_query.compile(db_driver::mariadb);
    }));

    // MariaDB counts characters, PostgreSQL counts bytes: 64 two-byte "é"
    // fit MariaDB but not PostgreSQL's 63 bytes.
    std::string accented{};
    for (int i = 0; i < 64; ++i) {
        accented += "\xC3\xA9";
    }
    db_query accented_query;
    accented_query.select(db_expr::call(accented)).from("events");
    const auto accented_statement = accented_query.compile(db_driver::mariadb);
    EXPECT_TRUE(accented_statement.sql().find(accented) != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)accented_query.compile(db_driver::postgresql);
    }));
    db_query too_many_characters;
    too_many_characters.select(db_expr::call(accented + "x")).from("events");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)too_many_characters.compile(db_driver::mariadb);
    }));
    db_query ill_formed;
    ill_formed.select(db_expr::call("bad\xC3name")).from("events");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)ill_formed.compile(db_driver::mariadb);
    }));
}

TEST(Ruvia, db_query_dml_sources_distinct_and_query_joins_compile) {
    db_query source;
    source.select({db_expr::column("id", "s"), db_expr::column("name", "s")}).from("source", "s");

    db_query insert;
    insert.insert_into("archive", {"id", "name"}).insert_from(source).returning({db_expr::column("id")});
    const auto inserted = insert.compile(db_driver::postgresql);
    EXPECT_EQ(inserted.sql(), "INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\" RETURNING \"id\"");

    db_query update;
    update.update("target", "t").set("name", db_expr::value("changed"));
    update.update_from("source", "s").where(db_expr::binary(db_expr::column("id", "t"), op_type::equal, db_expr::column("id", "s")));
    const auto updated = update.compile(db_driver::postgresql);
    EXPECT_EQ(updated.sql(), "UPDATE \"target\" AS \"t\" SET \"name\" = $1 FROM \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query removed;
    removed.delete_from("target", "t").delete_using("source", "s");
    removed.where(db_expr::binary(db_expr::column("id", "t"), op_type::equal, db_expr::column("id", "s")));
    const auto deleted = removed.compile(db_driver::postgresql);
    EXPECT_EQ(deleted.sql(), "DELETE FROM \"target\" AS \"t\" USING \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query distinct;
    distinct.select({db_expr::column("account_id"), db_expr::column("id")}).distinct_on({db_expr::column("account_id")}).from("events");
    const auto distinct_statement = distinct.compile(db_driver::postgresql);
    EXPECT_EQ(distinct_statement.sql(), "SELECT DISTINCT ON (\"account_id\") \"account_id\", \"id\" FROM \"events\"");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)distinct.compile(db_driver::mariadb);
    }));

    db_query joined;
    joined.select(db_selection::star("base")).from("base", "base");
    const auto join_on = db_expr::binary(db_expr::column("id", "base"), op_type::equal, db_expr::column("id", "derived"));
    joined.join(db_join_type::left, source, join_on, "derived");
    const std::array<std::string_view, 1> using_columns{"id"};
    joined.join(db_join_type::inner, "same_ids", {}, "same_ids", using_columns);
    const auto joined_statement = joined.compile(db_driver::postgresql);
    EXPECT_EQ(joined_statement.sql(), "SELECT \"base\".* FROM \"base\" AS \"base\" LEFT JOIN (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\") AS \"derived\" ON (\"base\".\"id\" = \"derived\".\"id\") INNER JOIN \"same_ids\" AS \"same_ids\" USING (\"id\")");

    db_query function_source;
    const auto function = db_expr::call("jsonb_each", {db_expr::column("payload")});
    const db_source_column function_columns[]{{.name_ = "key"}, {.name_ = "value"}};
    function_source.select(db_selection::star("kv")).from_function(function, "kv", {.lateral_ = true, .with_ordinality_ = true, .columns_ = function_columns});
    const auto function_statement = function_source.compile(db_driver::postgresql);
    EXPECT_TRUE(function_statement.sql().find("FROM LATERAL \"jsonb_each\"(\"payload\") WITH ORDINALITY AS \"kv\" (\"key\", \"value\")") != std::string_view::npos);
}

TEST(Ruvia, db_query_bulk_upsert_conditions_and_returning) {
    db_query query;
    const std::array<std::string_view, 2> columns{"id", "revision"};
    query.insert_into("device", columns, "d");
    const std::array first{db_expr::value(db_value(7)), db_expr::value(db_value(2))};
    const std::array second{db_expr::value(db_value(8)), db_expr::default_value()};
    query.values(first).values(second);
    const auto revision = db_expr::column("revision", "excluded");
    const borrowed_text conflict_columns[]{"id"};
    const db_assignment updates[]{{"revision", revision}};
    query.on_conflict({.conflict_target_ = conflict_columns, .update_set_ = updates, .update_where_ = revision > db_expr::column("revision", "d")});
    const std::array<db_selection, 1> returned{db_expr::column("id")};
    query.returning(returned);
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "INSERT INTO \"device\" AS \"d\" (\"id\", \"revision\") VALUES ($1, $2), ($3, DEFAULT) ON CONFLICT (\"id\") DO UPDATE SET \"revision\" = \"excluded\".\"revision\" WHERE (\"excluded\".\"revision\" > \"d\".\"revision\") RETURNING \"id\"");
    EXPECT_TRUE(statement.returns_rows());
    EXPECT_EQ(statement.params().size(), std::size_t{3});
}

TEST(Ruvia, db_query_mariadb_returning_on_insert_and_delete_but_not_update) {
    db_query upsert;
    const std::array<std::string_view, 2> columns{"id", "revision"};
    upsert.insert_into("device", columns);
    const std::array row{db_expr::value(db_value(7)), db_expr::value(db_value(2))};
    upsert.values(row);
    const db_assignment updates[]{{"revision", db_expr::excluded("revision")}};
    upsert.on_conflict({.update_set_ = updates, .any_unique_key_ = true});
    const std::array<db_selection, 2> returned{db_expr::column("id"), db_expr::column("revision").as("rev")};
    upsert.returning(returned);
    const auto inserted = upsert.compile(db_driver::mariadb);
    EXPECT_EQ(inserted.sql(), "INSERT INTO `device` (`id`, `revision`) VALUES (?, ?) ON DUPLICATE KEY UPDATE `revision` = VALUES(`revision`) RETURNING `id`, `revision` AS `rev`");
    EXPECT_TRUE(inserted.returns_rows());

    db_query removed;
    removed.delete_from("device").where(db_expr::binary(db_expr::column("id"), op_type::equal, db_expr::value(db_value(7))));
    const std::array<db_selection, 1> removed_columns{db_expr::column("id")};
    removed.returning(removed_columns);
    const auto deleted = removed.compile(db_driver::mariadb);
    EXPECT_EQ(deleted.sql(), "DELETE FROM `device` WHERE (`id` = ?) RETURNING `id`");

    db_query updated;
    updated.update("device").set("revision", db_expr::value(db_value(3)));
    const std::array<db_selection, 1> updated_columns{db_expr::column("id")};
    updated.returning(updated_columns);
    const auto updated_postgres = updated.compile(db_driver::postgresql);
    EXPECT_TRUE(updated_postgres.returns_rows());
    EXPECT_TRUE(testing::throws_on([&] {
        (void)updated.compile(db_driver::mariadb);
    }));
}

TEST(Ruvia, db_query_set_operations_preserve_grouping_and_nested_limits) {
    db_query first, second, third;
    first.select(db_expr::value(db_value(1)));
    second.select(db_expr::value(db_value(2))).limit(1);
    third.select(db_expr::value(db_value(3)));
    first.combine(db_set_operation::union_all, second).combine(db_set_operation::intersect, third).limit(4);
    const auto statement = first.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "((SELECT $1) UNION ALL (SELECT $2 LIMIT $3)) INTERSECT (SELECT $4) LIMIT $5");
    EXPECT_EQ(statement.params().size(), std::size_t{5});
}

TEST(Ruvia, db_query_mariadb_uses_its_own_placeholders_and_null_ordering) {
    db_query query;
    const auto name = db_expr::column("name");
    query.select(db_expr::binary(name, op_type::concat, db_expr::value(db_value("suffix")))).from("user");
    query.order_by(name, order_direction::asc, db_nulls_order::last).offset(2);
    const auto statement = query.compile(db_driver::mariadb);
    EXPECT_EQ(statement.sql(), "SELECT CONCAT(`name`, ?) FROM `user` ORDER BY (`name` IS NULL) ASC, `name` ASC LIMIT 18446744073709551615 OFFSET ?");
    query.where(db_expr::binary(name, op_type::i_like, db_expr::value(db_value("needle"))));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)query.compile(db_driver::mariadb);
    }));
}

TEST(Ruvia, db_query_copies_foreign_expressions_and_rejects_incompatible_clauses) {
    db_query first, second;
    const auto foreign = db_expr::column("id").expression(first);
    EXPECT_FALSE(testing::throws_on([&] {
        second.where(foreign);
    }));
    second.insert_into("device");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)second.compile(db_driver::postgresql);
    }));
    db_query invalid_join;
    invalid_join.from("one").join(db_join_type::left, "two");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)invalid_join.compile(db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_data_modifications_require_top_level_or_cte_placement) {
    db_query removed;
    removed.delete_from("device").returning({db_expr::column("id")});
    db_query query;
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_expr::subquery(removed);
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.from(removed, "removed");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.join(db_join_type::cross, removed, {}, "removed");
    }));
    db_query insert;
    insert.insert_into("archive", {"id"});
    EXPECT_TRUE(testing::throws_on([&] {
        insert.insert_from(removed);
    }));
    query.with("removed", removed).from("removed");
    EXPECT_EQ(query.compile(db_driver::postgresql).returns_rows(), true);
    db_query nested;
    EXPECT_TRUE(testing::throws_on([&] {
        nested.from(query, "nested");
    }));
    nested.with("nested", query).from("nested");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)nested.compile(db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_set_operands_cannot_hide_write_ctes_from_subquery_admission) {
    db_query removed;
    removed.delete_from("device").returning({db_expr::column("id")});
    db_query branch;
    branch.select(db_expr::value(1)).with("removed", removed);
    db_query combined;
    combined.select(db_expr::value(0)).combine(db_set_operation::union_all, branch);
    EXPECT_TRUE(combined.has_writes());
    EXPECT_TRUE(combined.clone().has_writes());
    db_query moved(std::move(combined));
    EXPECT_TRUE(moved.has_writes());
    for (unsigned operation = 0; operation != 4; ++operation) {
        db_query outer;
        outer.select(db_expr::value(7)).from("retained");
        bool rejected = false;
        try {
            switch (operation) {
                case 0:
                    outer.from(moved, "hidden");
                    break;
                case 1:
                    (void)db_expr::subquery(moved);
                    break;
                case 2:
                    outer.join(db_join_type::cross, moved, {}, "hidden");
                    break;
                case 3: {
                    db_query insert;
                    insert.insert_into("archive", {"id"}).insert_from(moved);
                    break;
                }
            }
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        if (operation != 3) {
            EXPECT_TRUE(!outer.has_writes());
            const auto statement = outer.compile(db_driver::postgresql, db_parameter_mode::literal);
            EXPECT_EQ(statement.sql(), "SELECT 7 FROM \"retained\"");
        }
    }
}

TEST(Ruvia, db_predicate_composes_typed_fields_and_literal_parameters) {
    std::string name(150, 'a');
    auto condition = (entity_type::column<"id">() >= 7) && (entity_type::column<"name">() == name);
    name.assign(150, 'z');
    db_query query;
    query.from("device").where(condition.expression(query));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT * FROM \"device\" WHERE ((\"device\".\"id\" >= $1) AND (\"device\".\"name\" = $2))");
}

RUVIA_DB_ENTITY(tagged_type, "tagged", db_column<"id", std::int64_t>, db_column<"tags", std::vector<std::string>>)
RUVIA_DB_ENTITY(uuids_type, "uuid_tags", db_column<"tags", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::uuid}>)
RUVIA_DB_ENTITY(codes_type, "codes", db_column<"values", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::varchar, .length_ = 32}>)

TEST(Ruvia, db_predicate_between_and_array_operators_bind_owned_typed_values) {
    std::array<std::string, 2> input{std::string(200, 'a'), std::string("zone-b")};
    auto condition = tagged_type::column<"id">().between(3, 8) && tagged_type::column<"tags">().array_contains(std::span<const std::string>(input));
    input[0].assign(200, 'z');
    db_query query;
    query.from("tagged").where(condition.expression(query));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT * FROM \"tagged\" WHERE ((\"tagged\".\"id\" BETWEEN $1 AND $2) AND (\"tagged\".\"tags\" @> CAST(ARRAY[$3, $4] AS TEXT[])))");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)query.compile(db_driver::mariadb);
    }));

    db_query contained;
    auto subset = tagged_type::column<"tags">().array_contained_by<std::string_view>({});
    contained.from("tagged").where(subset.expression(contained));
    const auto empty = contained.compile(db_driver::postgresql);
    EXPECT_EQ(empty.sql(), "SELECT * FROM \"tagged\" WHERE (\"tagged\".\"tags\" <@ CAST(ARRAY[] AS TEXT[]))");
    EXPECT_TRUE(empty.params().empty());

    db_query overlap;
    auto any = uuids_type::column<"tags">().array_overlap({"12345678-1234-1234-1234-123456789abc"});
    overlap.from("uuid_tags").where(any.expression(overlap));
    const auto typed = overlap.compile(db_driver::postgresql);
    EXPECT_EQ(typed.sql(), "SELECT * FROM \"uuid_tags\" WHERE (\"uuid_tags\".\"tags\" && CAST(ARRAY[$1] AS UUID[]))");
    db_query codes;
    auto code = codes_type::column<"values">().array_contains({"zone-a"});
    codes.from("codes").where(code.expression(codes));
    const auto sized = codes.compile(db_driver::postgresql);
    EXPECT_TRUE(sized.sql().find("AS VARCHAR(32)[]") != std::string_view::npos);
}

TEST(Ruvia, db_query_binary_operator_families_cover_postgresql_and_mariadb_dialects) {
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
        binary_case{op_type::and_, " AND ", " AND "},
        binary_case{op_type::or_, " OR ", " OR "},
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
        const auto lhs = db_expr::column("lhs");
        const auto rhs = item.list_right_ ? db_expr::list({db_expr::value(1), db_expr::value(2)}) : db_expr::column("rhs");
        query.select(db_expr::binary(lhs, item.op_, rhs)).from("cq_operator_matrix");
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.postgres_token_) != std::string_view::npos);
        const auto maria = query.compile(db_driver::mariadb);
        EXPECT_TRUE(maria.sql().find(item.maria_token_) != std::string_view::npos);
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
        query.select(db_expr::binary(db_expr::column("lhs"), item.op_, db_expr::column("rhs"))).from("cq_operator_matrix");
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.token_) != std::string_view::npos);
        EXPECT_TRUE(testing::throws_on([&] {
            (void)query.compile(db_driver::mariadb);
        }));
    }

    db_query nulls;
    const auto id = db_expr::column("id");
    nulls.select({db_expr::binary(id, op_type::equal, db_expr::null()), db_expr::binary(db_expr::null(), op_type::not_equal, id), db_expr::binary(id, op_type::in, db_expr::list({})), db_expr::binary(id, op_type::not_in, db_expr::list({}))}).from("cq_operator_matrix");
    const auto null_statement = nulls.compile(db_driver::postgresql);
    EXPECT_TRUE(null_statement.sql().find(" IS NULL") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find(" IS NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find("FALSE") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find("TRUE") != std::string_view::npos);

    db_query invalid_in;
    invalid_in.select(db_expr::binary(db_expr::column("id"), op_type::in, db_expr::value(1))).from("cq_operator_matrix");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)invalid_in.compile(db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_unary_operators_and_parameter_modes_cover_all_semantics) {
    struct unary_case final {
        db_unary_operator op_;
        std::string_view token_;
    };
    const std::array cases{
        unary_case{db_unary_operator::not_, "NOT"},
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
        query.select(db_expr::unary(item.op_, db_expr::column("value"))).from("cq_unary_matrix");
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.token_) != std::string_view::npos);
        const auto maria = query.compile(db_driver::mariadb);
        EXPECT_TRUE(maria.sql().find(item.token_) != std::string_view::npos);
    }

    db_query literal;
    const auto literal_value = db_expr::value(db_value("O'Reilly"));
    literal.select(literal_value).from("cq_unary_matrix");
    const auto bound = literal.compile(db_driver::postgresql, db_parameter_mode::bound);
    EXPECT_EQ(bound.params().size(), std::size_t{1});
    const auto rendered = literal.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(rendered.params().size(), std::size_t{0});
    EXPECT_TRUE(rendered.sql().find("E'O''Reilly'") != std::string_view::npos);
    const auto maria_rendered = literal.compile(db_driver::mariadb, db_parameter_mode::literal);
    EXPECT_TRUE(maria_rendered.sql().find("CONVERT(X'4f275265696c6c79' USING utf8mb4)") != std::string_view::npos);
    EXPECT_EQ(db_query::render_expression(literal_value, db_driver::postgresql), "E'O''Reilly'");

    EXPECT_TRUE(testing::throws_on([] {
        (void)db_query::render_expression(db_expr::unary(db_unary_operator::not_, {}), db_driver::postgresql);
    }));
    EXPECT_TRUE(testing::throws_on([] {
        (void)db_query::render_expression(db_expr::binary({}, op_type::equal, db_expr::value(1)), db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_unary_minus_and_bit_not_keep_negative_literals_out_of_comments) {
    const auto negated = db_expr::unary(db_unary_operator::negate, db_expr::value(-5));
    const auto inverted = db_expr::unary(db_unary_operator::bit_not, db_expr::value(-5));
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        const auto negated_sql = db_query::render_expression(negated, driver);
        EXPECT_EQ(negated_sql, "(- -5)");
        EXPECT_TRUE(negated_sql.find("--") == std::string_view::npos);
        EXPECT_EQ(db_query::render_expression(inverted, driver), driver == db_driver::postgresql ? "(~ (-5)::int8)" : "(~ -5)");
    }
}

RUVIA_DB_ENTITY(cq_device_type, "cq_device", db_column<"id", std::int64_t>, db_column<"name", std::string>, db_column<"tags", std::vector<std::string>>)

TEST(Ruvia, db_predicate_public_overloads_cover_comparisons_membership_nulls_and_aliases) {
    using tagged_type = cq_device_type;
    EXPECT_EQ(tagged_type::column<"id">().name(), std::string_view("id"));

    const auto compile = [](db_predicate predicate, db_driver driver) {
        db_query query;
        query.from("cq_device", "d").where(predicate.expression(query, "cq_device", "d"));
        return query.compile(driver);
    };
    const auto check_comparison = [&](db_predicate predicate, std::string_view token) {
        const auto statement = compile(std::move(predicate), db_driver::postgresql);
        EXPECT_TRUE(statement.sql().find(token) != std::string_view::npos);
        EXPECT_TRUE(statement.sql().find("\"d\".\"id\"") != std::string_view::npos);
    };
    check_comparison(tagged_type::column<"id">() == 1, " = ");
    check_comparison(tagged_type::column<"id">() != 1, " <> ");
    check_comparison(tagged_type::column<"id">() < 1, " < ");
    check_comparison(tagged_type::column<"id">() <= 1, " <= ");
    check_comparison(tagged_type::column<"id">() > 1, " > ");
    check_comparison(tagged_type::column<"id">() >= 1, " >= ");

    const auto like = compile(tagged_type::column<"name">().like("A%"), db_driver::postgresql);
    EXPECT_TRUE(like.sql().find(" LIKE ") != std::string_view::npos);
    const auto ilike = compile(tagged_type::column<"name">().ilike("a%"), db_driver::postgresql);
    EXPECT_TRUE(ilike.sql().find(" ILIKE ") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)compile(tagged_type::column<"name">().ilike("a%"), db_driver::mariadb);
    }));
    EXPECT_TRUE(compile_sql([&] {
        return compile(tagged_type::column<"id">().is_null(), db_driver::postgresql);
    }).find(" IS NULL") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] {
        return compile(tagged_type::column<"id">().is_not_null(), db_driver::postgresql);
    }).find(" IS NOT NULL") != std::string_view::npos);

    const std::array ids{1, 2, 3};
    const auto in_span = compile(tagged_type::column<"id">().in(std::span<const int>(ids)), db_driver::postgresql);
    EXPECT_TRUE(in_span.sql().find(" IN ($1, $2, $3)") != std::string_view::npos);
    const auto in_list = compile(tagged_type::column<"id">().in({4, 5}), db_driver::postgresql);
    EXPECT_TRUE(in_list.sql().find(" IN ($1, $2)") != std::string_view::npos);
    const std::span<const int> no_ids;
    const auto empty_in = compile(tagged_type::column<"id">().in(no_ids), db_driver::postgresql);
    EXPECT_TRUE(empty_in.sql().find("FALSE") != std::string_view::npos);

    const auto between = compile(tagged_type::column<"id">().between(2, 8), db_driver::postgresql);
    EXPECT_TRUE(between.sql().find(" BETWEEN ") != std::string_view::npos);
    const std::array<std::string_view, 2> tag_values{"one", "two"};
    const auto contains = compile(tagged_type::column<"tags">().array_contains(std::span<const std::string_view>(tag_values)), db_driver::postgresql);
    EXPECT_TRUE(contains.sql().find(" @> CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto contained = compile(tagged_type::column<"tags">().array_contained_by({"one", "two"}), db_driver::postgresql);
    EXPECT_TRUE(contained.sql().find(" <@ CAST(ARRAY[$1, $2]") != std::string_view::npos);
    const auto overlap = compile(tagged_type::column<"tags">().array_overlap({"two"}), db_driver::postgresql);
    EXPECT_TRUE(overlap.sql().find(" && CAST(ARRAY[$1]") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)compile(tagged_type::column<"tags">().array_contains({"one"}), db_driver::mariadb);
    }));

    auto combined = (tagged_type::column<"id">() == 1) || !(tagged_type::column<"name">().like("%x%"));
    const auto combined_statement = compile(std::move(combined), db_driver::postgresql);
    EXPECT_TRUE(combined_statement.sql().find(" OR ") != std::string_view::npos);
    EXPECT_TRUE(combined_statement.sql().find("NOT") != std::string_view::npos);

    db_predicate empty;
    db_query query;
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(empty.expression(query).empty());
    db_predicate owned = tagged_type::column<"id">() == 1;
    EXPECT_TRUE(!owned.empty());
    db_predicate moved = std::move(owned);
    EXPECT_TRUE(owned.empty());
    EXPECT_TRUE(owned.expression(query).empty());
    db_predicate reassigned;
    reassigned = std::move(moved);
    EXPECT_TRUE(moved.empty());
    EXPECT_TRUE(moved.expression(query).empty());
    EXPECT_TRUE(!reassigned.empty());
    db_predicate empty_and;
    db_predicate empty_or;
    EXPECT_TRUE(testing::throws_on([&] {
        (void)(std::move(empty_and) && (tagged_type::column<"id">() == 1));
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)(std::move(empty_or) || (tagged_type::column<"id">() == 1));
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)!std::move(empty);
    }));
}

TEST(Ruvia, db_predicate_moves_keep_expression_with_its_owner) {
    {
        auto source = entity_type::column<"name">() == std::string(200, 'x');
        auto destination = entity_type::column<"id">() == 9;
        {
            db_predicate moved(std::move(source));
            destination = std::move(moved);
            EXPECT_TRUE(moved.empty());
        }
        {
            db_query query;
            query.from("device").where(destination.expression(query));
            const auto statement = query.compile(db_driver::postgresql);
            EXPECT_EQ(statement.sql(), "SELECT * FROM \"device\" WHERE (\"device\".\"name\" = $1)");
            EXPECT_EQ(statement.params().size(), std::size_t{1});
        }
        destination = db_predicate{};
        db_query query;
        EXPECT_TRUE(source.empty());
        EXPECT_TRUE(source.expression(query).empty());
        EXPECT_TRUE(testing::throws_on([&] {
            (void)!std::move(source);
        }));
        source = entity_type::column<"id">() == 1;
        query.from("device").where(source.expression(query));
        const auto statement = query.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), "SELECT * FROM \"device\" WHERE (\"device\".\"id\" = $1)");
    }
}

TEST(Ruvia, db_query_clause_source_and_join_methods_cover_state_and_dialect_rules) {
    db_query query;
    const auto id = db_expr::column("id", "e");
    const auto kind = db_expr::column("kind", "e");
    query.select(db_selection::star("e")).add_select(db_expr::column("id", "e").as("selected_id")).from("cq_events", "e").where(db_expr::binary(id, op_type::greater, db_expr::value(0))).and_where(db_expr::binary(kind, op_type::not_equal, db_expr::value("archived"))).or_where(db_expr::binary(id, op_type::equal, db_expr::value(99))).group_by({id}).add_group_by(kind).having(db_expr::binary(db_expr::aggregate("count", {db_expr::star()}), op_type::greater, db_expr::value(0))).and_having(db_expr::binary(kind, op_type::like, db_expr::value("%live%"))).order_by(id, order_direction::desc, db_nulls_order::first).add_order_by(kind, order_direction::asc, db_nulls_order::last).limit(10).offset(2).distinct();
    EXPECT_TRUE(query.has_where());
    EXPECT_TRUE(query.has_grouping());
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_TRUE(statement.sql().find("SELECT DISTINCT ") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("GROUP BY \"e\".\"id\", \"e\".\"kind\"") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("HAVING") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("NULLS FIRST") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("NULLS LAST") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("LIMIT") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("OFFSET") != std::string_view::npos);

    query.clear_order().limit(std::nullopt).offset(std::nullopt).distinct(false);
    const auto cleared = query.compile(db_driver::postgresql);
    EXPECT_TRUE(cleared.sql().find(" ORDER BY ") == std::string_view::npos);
    EXPECT_TRUE(cleared.sql().find(" LIMIT ") == std::string_view::npos);
    EXPECT_TRUE(cleared.sql().find(" OFFSET ") == std::string_view::npos);
    EXPECT_TRUE(cleared.sql().find("DISTINCT") == std::string_view::npos);
    query.limit(7).offset(3);
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min() + 5, std::int64_t{-1}}) {
        EXPECT_TRUE(testing::throws_on([&] {
            query.limit(invalid);
        }));
        EXPECT_TRUE(testing::throws_on([&] {
            query.offset(invalid);
        }));
    }
    for (const auto above_bigint : {static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1, (std::numeric_limits<std::uint64_t>::max)()}) {
        db_query pg_limit = query.clone();
        pg_limit.limit(above_bigint);
        EXPECT_THROW((void)pg_limit.compile(db_driver::postgresql), std::invalid_argument);
        db_query pg_offset = query.clone();
        pg_offset.offset(above_bigint);
        EXPECT_THROW((void)pg_offset.compile(db_driver::postgresql), std::invalid_argument);
    }
    const auto retained_pagination = query.compile(db_driver::postgresql);
    EXPECT_TRUE(retained_pagination.sql().find(" LIMIT $6 OFFSET $7") != std::string_view::npos);
    EXPECT_EQ(retained_pagination.params().size(), std::size_t{7});
    query.limit(std::nullopt).offset(std::nullopt);

    db_query derived;
    derived.select(db_expr::column("id")).from("cq_source");
    db_query source_options;
    const db_source_column derived_columns[]{{.name_ = "id"}};
    source_options.select(db_selection::star("d")).from(derived, "d", {.columns_ = derived_columns});
    const auto source_statement = source_options.compile(db_driver::postgresql);
    EXPECT_TRUE(source_statement.sql().find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"d\" (\"id\")") != std::string_view::npos);
    const auto maria_source_statement = source_options.compile(db_driver::mariadb);
    EXPECT_TRUE(maria_source_statement.sql().find("FROM (SELECT `id` FROM `cq_source`) AS `d` (`id`)") != std::string_view::npos);

    db_query record_source;
    const auto record_function = db_expr::call("jsonb_to_record", {db_expr::value(R"({"id":1})")});
    const db_source_column record_columns[]{{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}, {.name_ = "name", .type_ = {.data_type_ = db_data_type::varchar, .length_ = 8}}};
    record_source.select(db_selection::star("r")).from_function(record_function, "r", {.columns_ = record_columns});
    const auto record_statement = record_source.compile(db_driver::postgresql);
    EXPECT_TRUE(record_statement.sql().find("AS \"r\" (\"id\" INTEGER, \"name\" VARCHAR(8))") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)record_source.compile(db_driver::mariadb);
    }));

    db_query lateral;
    lateral.select(db_selection::star("d")).from(derived, "d", {.lateral_ = true});
    EXPECT_TRUE(compile_sql([&] {
        return lateral.compile(db_driver::postgresql);
    }).find("FROM LATERAL") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)lateral.compile(db_driver::mariadb);
    }));
    db_query bad_ordinality;
    bad_ordinality.select(db_selection::star("d")).from(derived, "d", {.with_ordinality_ = true});
    EXPECT_TRUE(testing::throws_on([&] {
        (void)bad_ordinality.compile(db_driver::postgresql);
    }));

    const std::array joins{
        std::pair{db_join_type::inner, std::string_view(" INNER JOIN ")},
        std::pair{db_join_type::left, std::string_view(" LEFT JOIN ")},
        std::pair{db_join_type::right, std::string_view(" RIGHT JOIN ")},
        std::pair{db_join_type::full, std::string_view(" FULL JOIN ")},
    };
    for (const auto& [type, token] : joins) {
        db_query joined;
        joined.select(db_selection::star("a")).from("cq_a", "a");
        const auto on = db_expr::binary(db_expr::column("id", "a"), op_type::equal, db_expr::column("id", "b"));
        joined.join(type, "cq_b", on, "b");
        const auto postgres = joined.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(token) != std::string_view::npos);
        if (type == db_join_type::full) {
            EXPECT_TRUE(testing::throws_on([&] {
                (void)joined.compile(db_driver::mariadb);
            }));
        } else {
            const auto maria = joined.compile(db_driver::mariadb);
            EXPECT_TRUE(maria.sql().find(token) != std::string_view::npos);
        }
    }
    db_query cross;
    cross.select(db_selection::star("a")).from("cq_a", "a").join(db_join_type::cross, "cq_b", {}, "b");
    EXPECT_TRUE(compile_sql([&] {
        return cross.compile(db_driver::postgresql);
    }).find(" CROSS JOIN ") != std::string_view::npos);
    cross.join(db_join_type::cross, "cq_c", db_expr::column("id"), "c");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)cross.compile(db_driver::postgresql);
    }));
}

TEST(Ruvia, db_query_dml_and_conflict_overloads_cover_postgresql_and_mariadb) {
    db_query insert;
    const std::array<std::string_view, 2> columns{"id", "name"};
    insert.insert_into("cq_devices", std::span<const std::string_view>(columns), "d");
    const std::array row{db_expr::value(1), db_expr::value("first")};
    insert.values(std::span<const db_expr>(row));
    const auto inserted = insert.compile(db_driver::postgresql);
    EXPECT_TRUE(inserted.sql().find("INSERT INTO \"cq_devices\" AS \"d\"") != std::string_view::npos);
    EXPECT_TRUE(!inserted.returns_rows());

    db_query defaults;
    defaults.insert_into("cq_devices");
    const auto default_pg = defaults.compile(db_driver::postgresql);
    EXPECT_TRUE(default_pg.sql().find("DEFAULT VALUES") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] {
        return defaults.compile(db_driver::mariadb);
    }).find("() VALUES ()") != std::string_view::npos);

    db_query source;
    source.select({db_expr::column("id"), db_expr::column("name")}).from("cq_source");
    db_query insert_from;
    insert_from.insert_into("cq_devices", {"id", "name"}).insert_from(source);
    EXPECT_TRUE(compile_sql([&] {
        return insert_from.compile(db_driver::postgresql);
    }).find("SELECT \"id\", \"name\"") != std::string_view::npos);

    db_query do_nothing;
    do_nothing.insert_into("cq_devices", {"id", "name"}).values({db_expr::value(1), db_expr::value("x")});
    const borrowed_text conflict_columns[]{"id"};
    do_nothing.on_conflict({.conflict_target_ = conflict_columns, .do_nothing_ = true});
    EXPECT_TRUE(compile_sql([&] {
        return do_nothing.compile(db_driver::postgresql);
    }).find("ON CONFLICT (\"id\") DO NOTHING") != std::string_view::npos);

    db_query constraint;
    constraint.insert_into("cq_devices", {"id", "name"}).values({db_expr::value(1), db_expr::value("x")});
    constraint.on_conflict({.constraint_ = "cq_devices_pkey", .do_nothing_ = true});
    EXPECT_TRUE(compile_sql([&] {
        return constraint.compile(db_driver::postgresql);
    }).find("ON CONSTRAINT \"cq_devices_pkey\" DO NOTHING") != std::string_view::npos);

    db_query targeted;
    targeted.insert_into("cq_devices", {"id", "name"}).values({db_expr::value(1), db_expr::value("x")});
    const auto active = db_expr::column("active") == db_expr::value(true);
    const db_assignment targeted_updates[]{{"name", db_expr::excluded("name")}};
    targeted.on_conflict({.conflict_target_ = conflict_columns, .index_predicate_ = active, .update_set_ = targeted_updates, .update_where_ = db_expr::column("name") != db_expr::excluded("name")});
    const auto targeted_statement = targeted.compile(db_driver::postgresql);
    EXPECT_TRUE(targeted_statement.sql().find("ON CONFLICT (\"id\") WHERE (\"active\" = $3) DO UPDATE SET") != std::string_view::npos);
    EXPECT_TRUE(targeted_statement.sql().find("WHERE (\"name\" <> ") != std::string_view::npos);
    EXPECT_TRUE(targeted_statement.sql().find("excluded.\"name\")") != std::string_view::npos || targeted_statement.sql().find("\"excluded\".\"name\")") != std::string_view::npos);

    db_query maria_upsert;
    maria_upsert.insert_into("cq_devices", {"id", "name"}).values({db_expr::value(1), db_expr::value("x")});
    const db_assignment maria_updates[]{{"name", db_expr::excluded("name")}};
    maria_upsert.on_conflict({.update_set_ = maria_updates, .any_unique_key_ = true});
    const auto maria_statement = maria_upsert.compile(db_driver::mariadb);
    EXPECT_TRUE(maria_statement.sql().find("ON DUPLICATE KEY UPDATE \"name\"") == std::string_view::npos);
    EXPECT_TRUE(maria_statement.sql().find("ON DUPLICATE KEY UPDATE `name` = VALUES(`name`)") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)maria_upsert.compile(db_driver::postgresql);
    }));
    db_query invalid_conflict;
    invalid_conflict.insert_into("cq_devices", {"id"}).values({db_expr::value(1)});
    invalid_conflict.on_conflict({.conflict_target_ = conflict_columns, .constraint_ = "other", .do_nothing_ = true});
    EXPECT_TRUE(testing::throws_on([&] {
        (void)invalid_conflict.compile(db_driver::postgresql);
    }));

    db_query update;
    update.update("cq_devices", "d").set("name", db_expr::value("updated"));
    update.set("name", db_expr::value("updated-again"));
    db_query update_from;
    update_from.select(db_expr::column("id")).from("cq_source");
    update.update_from(update_from, "s").where(db_expr::binary(db_expr::column("id", "d"), op_type::equal, db_expr::column("id", "s")));
    EXPECT_TRUE(compile_sql([&] {
        return update.compile(db_driver::postgresql);
    }).find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);

    db_query remove;
    remove.delete_from("cq_devices", "d");
    db_query delete_source;
    delete_source.select(db_expr::column("id")).from("cq_source");
    remove.delete_using(delete_source, "s").where(db_expr::binary(db_expr::column("id", "d"), op_type::equal, db_expr::column("id", "s")));
    EXPECT_TRUE(compile_sql([&] {
        return remove.compile(db_driver::postgresql);
    }).find("USING (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)remove.compile(db_driver::mariadb);
    }));

    db_query returning;
    returning.insert_into("cq_devices", {"id"}).values({db_expr::value(1)}).returning({db_expr::column("id")});
    const auto returning_statement = returning.compile(db_driver::postgresql);
    EXPECT_TRUE(returning_statement.returns_rows());
    const auto maria_returning = returning.compile(db_driver::mariadb);
    EXPECT_EQ(maria_returning.sql(), "INSERT INTO `cq_devices` (`id`) VALUES (?) RETURNING `id`");
}

TEST(Ruvia, db_query_lock_cte_and_set_operation_enumerations_cover_supported_and_rejected_paths) {
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
        query.select(db_selection::star("d")).from("cq_devices", "d").lock({.mode_ = item.mode_});
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_token_.empty()) {
            EXPECT_TRUE(testing::throws_on([&] {
                (void)query.compile(db_driver::mariadb);
            }));
        } else {
            EXPECT_TRUE(compile_sql([&] {
                return query.compile(db_driver::mariadb);
            }).find(item.maria_token_) != std::string_view::npos);
        }
    }
    db_query scoped_lock;
    const borrowed_text lock_tables[]{"d"};
    scoped_lock.select(db_selection::star("d")).from("cq_devices", "d").lock({.mode_ = db_row_lock::update, .nowait_ = true, .tables_ = lock_tables});
    EXPECT_TRUE(compile_sql([&] {
        return scoped_lock.compile(db_driver::postgresql);
    }).find("FOR UPDATE OF \"d\" NOWAIT") != std::string_view::npos);
    db_query skip_locked;
    skip_locked.select(db_selection::star()).from("cq_devices").lock({.skip_locked_ = true});
    EXPECT_TRUE(compile_sql([&] {
        return skip_locked.compile(db_driver::postgresql);
    }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] {
        return skip_locked.compile(db_driver::mariadb);
    }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    db_query cleared;
    cleared.select(db_selection::star()).from("cq_devices").lock({}).clear_lock();
    EXPECT_TRUE(compile_sql([&] {
        return cleared.compile(db_driver::postgresql);
    }).find(" FOR ") == std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        cleared.lock({.nowait_ = true, .skip_locked_ = true});
    }));
    db_query shared_skip;
    shared_skip.select(db_selection::star()).from("cq_devices").lock({.mode_ = db_row_lock::share, .skip_locked_ = true});
    EXPECT_TRUE(testing::throws_on([&] {
        (void)shared_skip.compile(db_driver::mariadb);
    }));

    db_query cte_body;
    cte_body.select(db_expr::value(1)).from("cq_seed");
    db_query recursive;
    const borrowed_text cte_columns[]{"n"};
    recursive.with("numbers", cte_body, {.recursive_ = true, .materialization_ = db_materialization::not_materialized, .columns_ = cte_columns}).select(db_expr::column("n")).from("numbers");
    const auto recursive_statement = recursive.compile(db_driver::postgresql);
    EXPECT_TRUE(recursive_statement.sql().find("WITH RECURSIVE \"numbers\" (\"n\") AS NOT MATERIALIZED") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)recursive.compile(db_driver::mariadb);
    }));
    db_query duplicate_cte;
    duplicate_cte.with("one", cte_body);
    EXPECT_TRUE(testing::throws_on([&] {
        duplicate_cte.with("one", cte_body);
    }));

    struct set_case final {
        db_set_operation operation_;
        std::string_view postgres_token_;
        bool maria_supported_;
    };
    const std::array set_cases{
        set_case{db_set_operation::union_, " UNION ", true},
        set_case{db_set_operation::union_all, " UNION ALL ", true},
        set_case{db_set_operation::intersect, " INTERSECT ", true},
        set_case{db_set_operation::intersect_all, " INTERSECT ALL ", false},
        set_case{db_set_operation::except, " EXCEPT ", true},
        set_case{db_set_operation::except_all, " EXCEPT ALL ", false},
    };
    for (const auto& item : set_cases) {
        db_query first;
        first.select(db_expr::value(1));
        db_query second;
        second.select(db_expr::value(2));
        first.combine(item.operation_, second);
        EXPECT_TRUE(compile_sql([&] {
            return first.compile(db_driver::postgresql);
        }).find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_supported_) {
            EXPECT_TRUE(compile_sql([&] {
                return first.compile(db_driver::mariadb);
            }).find(item.postgres_token_) != std::string_view::npos);
        } else {
            EXPECT_TRUE(testing::throws_on([&] {
                (void)first.compile(db_driver::mariadb);
            }));
        }
    }
}

TEST(Ruvia, db_query_window_frame_and_date_part_enumerations_cover_each_value) {
    const std::array frames{db_window_frame::rows, db_window_frame::range, db_window_frame::groups};
    for (const auto frame : frames) {
        db_query query;
        query.select(db_expr::call("row_number").over({.frame_ = db_window_frame_options{.kind_ = frame, .start_ = {db_frame_boundary::unbounded_preceding, 0}, .end_ = {db_frame_boundary::current_row, 0}}})).from("cq_window");
        const auto postgres = query.compile(db_driver::postgresql);
        const auto token = frame == db_window_frame::rows ? "ROWS" : (frame == db_window_frame::range ? "RANGE" : "GROUPS");
        EXPECT_TRUE(postgres.sql().find(token) != std::string_view::npos);
        if (frame == db_window_frame::groups) {
            EXPECT_TRUE(testing::throws_on([&] {
                (void)query.compile(db_driver::mariadb);
            }));
        } else {
            EXPECT_TRUE(compile_sql([&] {
                return query.compile(db_driver::mariadb);
            }).find(token) != std::string_view::npos);
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
        const auto start_value = start == db_frame_boundary::preceding ? 2U : (start == db_frame_boundary::following ? 3U : 0U);
        const auto end_value = end == db_frame_boundary::preceding ? 2U : (end == db_frame_boundary::following ? 3U : 0U);
        query.select(db_expr::call("row_number").over({.frame_ = db_window_frame_options{.kind_ = db_window_frame::rows, .start_ = {start, start_value}, .end_ = {end, end_value}}})).from("cq_window");
        EXPECT_TRUE(compile_sql([&] {
            return query.compile(db_driver::postgresql);
        }).find(token) != std::string_view::npos);
    }
    db_query invalid_boundary;
    invalid_boundary.select(db_expr::call("row_number").over({.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::current_row, 1}}})).from("cq_window");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)invalid_boundary.compile(db_driver::postgresql);
    }));
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(testing::throws_on([&] {
            (void)db_window_boundary{db_frame_boundary::preceding, invalid};
        }));
    }
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_window_boundary{db_frame_boundary::following, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1};
    }));
    db_query reversed;
    reversed.select(db_expr::call("row_number").over({.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::following, 0}, .end_ = {db_frame_boundary::preceding, 0}}})).from("cq_window");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)reversed.compile(db_driver::postgresql);
    }));

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
        query.select(db_expr::column("created_at").extract(item.part_)).from("cq_window");
        EXPECT_TRUE(compile_sql([&] {
            return query.compile(db_driver::postgresql);
        }).find(item.token_) != std::string_view::npos);
        if (item.maria_supported_) {
            EXPECT_TRUE(compile_sql([&] {
                return query.compile(db_driver::mariadb);
            }).find(item.token_) != std::string_view::npos);
        } else {
            EXPECT_TRUE(testing::throws_on([&] {
                (void)query.compile(db_driver::mariadb);
            }));
        }
    }
}

TEST(Ruvia, db_query_expression_ownership_import_subquery_cast_and_cache_methods_are_observable) {
    db_query source;
    const auto qualified = db_expr::column("id", "source").expression(source);
    db_query target;
    const auto remapped = db_expr(qualified).expression(target, "source", "target");
    target.select(db_expr(remapped)).from("cq_devices", "target");
    EXPECT_EQ(compile_sql([&] {
        return target.compile(db_driver::postgresql);
    }),
        "SELECT \"target\".\"id\" FROM \"cq_devices\" AS \"target\"");
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_expr().expression(target, "source", "target");
    }));

    db_query child;
    child.select(db_expr::value(1)).from("cq_devices");
    db_query nested;
    const auto exists = db_predicate::exists(child);
    const auto scalar = db_expr::subquery(child);
    nested.select({db_expr(exists), scalar}).from("cq_devices");
    const auto nested_statement = nested.compile(db_driver::postgresql);
    EXPECT_TRUE(nested_statement.sql().find("EXISTS (SELECT $1 FROM \"cq_devices\")") != std::string_view::npos);
    EXPECT_TRUE(nested_statement.sql().find("(SELECT $2 FROM \"cq_devices\")") != std::string_view::npos);
    db_query dml;
    dml.delete_from("cq_devices").returning({db_expr::column("id")});
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_expr::subquery(dml);
    }));

    db_query typed;
    const auto number = db_expr::value(1).cast(db_data_type::integer);
    const auto sized = db_expr::value("x").cast(db_type_definition{.data_type_ = db_data_type::varchar, .length_ = 8});
    typed.select({number, sized}).from("cq_devices");
    const auto typed_pg = typed.compile(db_driver::postgresql);
    EXPECT_TRUE(typed_pg.sql().find("CAST($1 AS INTEGER)") != std::string_view::npos);
    EXPECT_TRUE(typed_pg.sql().find("CAST($2 AS VARCHAR(8))") != std::string_view::npos);
    db_query named_type;
    named_type.select(db_expr::value(1).cast(db_type_definition{.custom_name_ = "cq_custom_type"})).from("cq_devices");
    EXPECT_TRUE(compile_sql([&] {
        return named_type.compile(db_driver::postgresql);
    }).find("CAST($1 AS \"cq_custom_type\")") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)named_type.compile(db_driver::mariadb);
    }));

    db_query named;
    const std::array<db_named_argument, 1> named_arguments{{db_named_argument{"precision", db_expr::value(2)}}};
    named.select(db_expr::call("round", {}, named_arguments)).from("cq_devices");
    EXPECT_TRUE(compile_sql([&] {
        return named.compile(db_driver::postgresql);
    }).find("\"precision\" => $1") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)named.compile(db_driver::mariadb);
    }));
    EXPECT_TRUE(testing::throws_on([] {
        const std::array<db_named_argument, 2> duplicate{{db_named_argument{"x", db_expr::value(1)}, db_named_argument{"x", db_expr::value(2)}}};
        (void)db_query::render_expression(db_expr::call("round", {}, duplicate), db_driver::postgresql);
    }));

    db_query state;
    EXPECT_TRUE(state.returns_rows());
    EXPECT_TRUE(!state.has_where());
    EXPECT_TRUE(!state.has_grouping());
    state.cache({.enabled_ = false}).cache({.duration_ = std::chrono::milliseconds(50)}).cache({.id_ = "cq-cache", .duration_ = std::chrono::milliseconds(25)});
    state.select(db_expr::value(1)).from("cq_devices");
    const auto state_statement = state.compile(db_driver::postgresql);
    EXPECT_TRUE(state_statement.params().size() == std::size_t{1});
    EXPECT_TRUE(testing::throws_on([&] {
        state.cache({.id_ = "bad", .duration_ = std::chrono::milliseconds(0)});
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        state.cache({.id_ = "bad", .duration_ = std::chrono::milliseconds(-1)});
    }));

    auto clone = state.clone();
    const auto clone_sql = compile_sql([&] {
        return clone.compile(db_driver::postgresql);
    });
    const auto state_sql = compile_sql([&] {
        return state.compile(db_driver::postgresql);
    });
    EXPECT_EQ(clone_sql, state_sql);
    db_query moved = std::move(clone);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)clone.returns_rows();
    }));
    db_query assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.returns_rows());
    EXPECT_TRUE(testing::throws_on([&] {
        (void)moved.returns_rows();
    }));

    db_query values;
    values.values({db_expr::value(1), db_expr::value(2)});
    EXPECT_TRUE(values.returns_rows());
    db_query dml_insert;
    dml_insert.insert_into("cq_devices", {"id"}).values({db_expr::value(1)});
    EXPECT_TRUE(!dml_insert.returns_rows());
    dml_insert.returning({db_expr::column("id")});
    EXPECT_TRUE(dml_insert.returns_rows());
}

RUVIA_DB_ENTITY(char_codes_type, "char_codes", db_column<"values", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::char_, .length_ = 8}>)

TEST(Ruvia, db_query_char_cast_and_array_predicate_render_for_supported_dialects) {
    db_query casts;
    casts.select(db_expr::value("x").cast(db_type_definition{.data_type_ = db_data_type::char_, .length_ = 64})).from("char_records");
    const auto postgres = casts.compile(db_driver::postgresql);
    EXPECT_TRUE(postgres.sql().find("CAST($1 AS CHAR(64))") != std::string_view::npos);

    const auto maria = casts.compile(db_driver::mariadb);
    EXPECT_TRUE(maria.sql().find("CAST(? AS CHAR(64))") != std::string_view::npos);

    db_query long_maria_cast;
    long_maria_cast.select(db_expr::value("x").cast(db_type_definition{.data_type_ = db_data_type::char_, .length_ = 256})).from("char_records");
    const auto long_maria_statement = long_maria_cast.compile(db_driver::mariadb);
    EXPECT_TRUE(long_maria_statement.sql().find("AS CHAR(256)") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        db_query invalid;
        invalid.select(db_expr::value("x").cast(db_type_definition{.data_type_ = db_data_type::char_, .length_ = 10485761})).from("char_records");
        (void)invalid.compile(db_driver::postgresql);
    }));

    db_query array;
    auto contains = char_codes_type::column<"values">().array_contains({"zone-a"});
    array.from("char_codes").where(contains.expression(array));
    const auto array_statement = array.compile(db_driver::postgresql);
    EXPECT_TRUE(array_statement.sql().find("AS CHAR(8)[]") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)array.compile(db_driver::mariadb);
    }));
}

TEST(Ruvia, db_query_span_overloads_cover_expression_and_statement_builders) {
    db_query query;
    const auto left = db_expr::column("left");
    const auto right = db_expr::column("right");
    const std::array<db_expr, 2> expressions{left, right};
    const auto call = db_expr::call("coalesce", std::span<const db_expr>(expressions));
    const auto aggregate = db_expr::aggregate("count", std::span<const db_expr>(expressions));
    const auto coalesced = db_expr::coalesce(std::span<const db_expr>(expressions));
    const auto greatest = db_expr::greatest(std::span<const db_expr>(expressions));
    const auto least = db_expr::least(std::span<const db_expr>(expressions));
    const db_tuple tuple{std::span<const db_expr>(expressions)};
    const auto list = db_expr::list(std::span<const db_expr>(expressions));
    const auto array_value = db_expr::array(std::span<const db_expr>(expressions));
    const std::array<db_case_branch, 1> branches{{{db_expr::binary(left, db_binary_operator::equal, db_expr::value(1)), right}}};
    const auto case_expression = db_expr::case_when(std::span<const db_case_branch>(branches), db_expr::value(0));
    const std::array<db_order_term, 1> order{{{left, order_direction::asc, db_nulls_order::default_}}};
    const auto within = aggregate.within_group(std::span<const db_order_term>(order));
    const std::array<db_selection, 2> selections{left, right};
    query.select(std::span<const db_selection>(selections)).add_select(call).from("cq_span").group_by(std::span<const db_expr>(&left, 1)).add_group_by(right).having(db_expr::unary(db_unary_operator::is_not_null, coalesced)).and_having(db_expr::binary(greatest, db_binary_operator::greater, least)).order_by(left).add_order_by(right);
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_TRUE(statement.sql().find("GROUP BY") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("HAVING") != std::string_view::npos);

    db_query distinct;
    distinct.select(db_expr::column("left"));
    const std::array<db_expr, 1> distinct_expressions{db_expr::column("left")};
    distinct.distinct_on(std::span<const db_expr>(distinct_expressions)).from("cq_span");
    EXPECT_TRUE(compile_sql([&] {
        return distinct.compile(db_driver::postgresql);
    }).find("DISTINCT ON") != std::string_view::npos);

    db_query dml;
    const std::array<std::string_view, 1> columns{"value"};
    const std::array<db_expr, 1> row{db_expr::value(1)};
    dml.insert_into("cq_span", std::span<const std::string_view>(columns)).values(std::span<const db_expr>(row));
    const std::array<db_selection, 1> returned{db_expr::column("value")};
    dml.returning(std::span<const db_selection>(returned));
    EXPECT_TRUE(dml.compile(db_driver::postgresql).returns_rows());

    (void)tuple;
    (void)list;
    (void)array_value;
    (void)case_expression;
    (void)within;
}

TEST(Ruvia, db_query_import_expression_rewrites_correlated_subquery_qualifiers) {
    db_query sub;
    sub.select(db_expr::value(1)).from("orders", "o").where(db_expr::binary(db_expr::column("user_id", "o"), db_binary_operator::equal, db_expr::column("id", "users")));
    db_query builder;
    const auto exists_expr = db_predicate::exists(sub);
    builder.select(db_expr(exists_expr));
    const auto original = compile_sql([&] {
        return builder.compile(db_driver::postgresql);
    });

    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(exists_expr.expression(main_query, "users", "u"));
    const auto compiled = main_query.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"o\" WHERE (\"o\".\"user_id\" = \"u\".\"id\"))");
    EXPECT_EQ(compile_sql([&] {
        return builder.compile(db_driver::postgresql);
    }),
        original);
}

TEST(Ruvia, db_query_import_expression_preserves_schema_qualified_local_bindings) {
    for (const std::string_view qualifier : {"users", "public.users"}) {
        db_query main_query;
        main_query.select(db_expr::column("id", "u")).from("users", "u");
        {
            db_query sub;
            sub.select(db_expr::value(1)).from("public.users").where(db_expr::binary(db_expr::column("id", qualifier), op_type::equal, db_expr::value(10)));
            const auto expression = db_predicate::exists(sub);
            main_query.where(expression.expression(main_query, qualifier, "u"));
        }
        const auto compiled = main_query.compile(db_driver::postgresql);
        const std::string expected(qualifier == "users" ? "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"public\".\"users\" WHERE (\"users\".\"id\" = $2))" : "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"public\".\"users\" WHERE (\"public\".\"users\".\"id\" = $2))");
        EXPECT_EQ(compiled.sql(), expected);
        EXPECT_EQ(compiled.params().size(), std::size_t{2});
    }
}

TEST(Ruvia, db_query_import_expression_rejects_captured_correlated_references_only) {
    db_query sub;
    sub.select(db_expr::value(1)).from("orders", "u").where(db_expr::binary(db_expr::column("user_id", "u"), op_type::equal, db_expr::column("id", "users")));
    db_query builder;
    const auto expression = db_predicate::exists(sub);
    builder.select(db_expr(expression));
    const auto original = compile_sql([&] {
        return builder.compile(db_driver::postgresql);
    });
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u");
    bool rejected = false;
    try {
        (void)expression.expression(main_query, "users", "u");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(compile_sql([&] {
        return builder.compile(db_driver::postgresql);
    }),
        original);

    // The abandoned correlated predicate still exists in the subquery's storage,
    // but does not appear in the imported query and must not cause a rejection.
    sub.where(db_expr::binary(db_expr::column("user_id", "u"), op_type::equal, db_expr::value(7)));
    main_query.where(db_predicate::exists(sub).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"u\" WHERE (\"u\".\"user_id\" = $2))");
}

TEST(Ruvia, db_query_import_expression_preserves_nested_source_shadowing) {
    db_query inner;
    inner.select(db_expr::value(1)).from("orders", "u").where(db_expr::binary(db_expr::column("user_id", "u"), op_type::equal, db_expr::column("id", "users")));
    db_query middle;
    middle.select(db_expr::column("id", "users")).from("public.users").where(db_predicate::exists(inner));
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(middle).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"public\".\"users\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"u\" WHERE (\"u\".\"user_id\" = \"users\".\"id\")))");
}

TEST(Ruvia, db_query_import_expression_maps_nested_correlations_without_crossing_local_scopes) {
    db_query inner;
    inner.select(db_expr::column("id", "users")).from("public.users");
    db_query middle;
    middle.select(db_expr::column("id", "users")).from("orders", "o").where(db_predicate::exists(inner));
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(middle).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"public\".\"users\"))");
}

TEST(Ruvia, db_query_import_expression_rejects_capture_by_an_enclosing_subquery) {
    db_query inner;
    inner.select(db_expr::column("id", "users")).from("items", "i");
    db_query middle;
    middle.select(db_expr::column("id", "u")).from("orders", "u").where(db_predicate::exists(inner));
    const auto expression = db_predicate::exists(middle);
    db_query main_query;
    bool rejected = false;
    try {
        (void)expression.expression(main_query, "users", "u");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, db_query_import_expression_keeps_set_operand_scopes_independent) {
    for (const std::string_view qualifier : {"users", "u"}) {
        db_query branch;
        branch.select(db_expr::column("id", "users")).from("orders", "o");
        db_query sub;
        sub.select(db_expr::column("id", qualifier));
        if (qualifier == "users") {
            sub.from("public.users");
        } else {
            sub.from("orders", "u");
        }
        sub.combine(db_set_operation::union_all, branch);
        db_query main_query;
        main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(sub).expression(main_query, "users", "u"));
        const std::string expected(qualifier == "users" ? "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS ((SELECT \"users\".\"id\" FROM \"public\".\"users\") UNION ALL (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\"))" : "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS ((SELECT \"u\".\"id\" FROM \"orders\" AS \"u\") UNION ALL (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\"))");
        EXPECT_EQ(compile_sql([&] {
            return main_query.compile(db_driver::postgresql);
        }),
            expected);
    }
}

TEST(Ruvia, db_query_import_expression_keeps_cte_body_scopes_independent) {
    db_query cte;
    cte.select(db_expr::column("id", "users")).from("orders", "o");
    db_query sub;
    sub.with("matched", cte).select(db_expr::column("id", "users")).from("public.users");
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(sub).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (WITH \"matched\" AS (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\") SELECT \"users\".\"id\" FROM \"public\".\"users\")");
}

TEST(Ruvia, db_query_import_expression_preserves_schema_qualified_join_bindings) {
    db_query sub;
    sub.select(db_expr::column("id", "users")).from("orders", "o").join(db_join_type::inner, "public.users", db_expr::binary(db_expr::column("user_id", "o"), op_type::equal, db_expr::column("id", "users")));
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(sub).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"orders\" AS \"o\" INNER JOIN \"public\".\"users\" ON (\"o\".\"user_id\" = \"users\".\"id\"))");
}

TEST(Ruvia, db_query_import_expression_rejects_qualified_table_and_join_target_capture) {
    for (const bool joined : {false, true}) {
        db_query sub;
        sub.select(db_expr::column("id", "users"));
        if (joined) {
            sub.from("orders", "o").join(db_join_type::cross, "items", {}, "u");
        } else {
            sub.from("public.u");
        }
        const auto expression = db_predicate::exists(sub);
        db_query main_query;
        bool rejected = false;
        try {
            (void)expression.expression(main_query, "users", "u");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, db_query_import_expression_rewrites_deep_correlated_references) {
    db_query inner;
    inner.select(db_expr::column("id", "users")).from("items", "i");
    db_query middle;
    middle.select(db_expr::column("id", "o")).from("orders", "o").where(db_predicate::exists(inner));
    db_query main_query;
    main_query.select(db_expr::column("id", "u")).from("users", "u").where(db_predicate::exists(middle).expression(main_query, "users", "u"));
    EXPECT_EQ(compile_sql([&] {
        return main_query.compile(db_driver::postgresql);
    }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"o\".\"id\" FROM \"orders\" AS \"o\" WHERE EXISTS (SELECT \"u\".\"id\" FROM \"items\" AS \"i\"))");
}

TEST(Ruvia, db_query_at_time_zone_and_using_join_render_and_validate) {
    db_query query;
    const auto local_ts = db_expr::column("created_at", "e").at_time_zone("Asia/Shanghai");
    const auto offset_ts = db_expr::column("created_at", "e").at_time_zone(db_expr::value("08:00:00").cast(db_data_type::interval));
    query.select({local_ts, offset_ts}).from("events", "e").join(db_join_type::left, "hourly_totals", {}, "h", {"hour"}).group_by({local_ts, offset_ts});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(),
        "SELECT (\"e\".\"created_at\" AT TIME ZONE $1::text), (\"e\".\"created_at\" AT TIME ZONE CAST($2 AS INTERVAL)) "
        "FROM \"events\" AS \"e\" LEFT JOIN \"hourly_totals\" AS \"h\" USING (\"hour\") "
        "GROUP BY (\"e\".\"created_at\" AT TIME ZONE $1::text), (\"e\".\"created_at\" AT TIME ZONE CAST($2 AS INTERVAL))");
    EXPECT_EQ(statement.params().size(), std::size_t{2});

    const auto literal = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_TRUE(literal.sql().find("(\"e\".\"created_at\" AT TIME ZONE E'Asia/Shanghai')") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)query.compile(db_driver::mariadb);
    }));
}

RUVIA_DB_ENTITY(user_entity, "users", db_column<"id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>, db_column<"department_id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .nullable_ = true}>, db_column<"nickname", std::string, db_column_options{.nullable_ = true}>, db_column<"active", bool>, db_column<"revision", std::int64_t>, db_column<"config", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>, db_column<"updated_at", std::string, db_column_options{.data_type_ = db_data_type::timestamp_tz}>)
RUVIA_DB_ENTITY(dept_entity, "departments", db_column<"id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>, db_column<"name", std::string>)

TEST(Ruvia, db_field_reference_and_predicate_cover_aliases_columns_json_and_owned_assignments) {
    static_assert(user_entity::column_name<"department_id">() == "department_id");
    EXPECT_EQ(user_entity::column<"id">("u").alias(), std::string_view("u"));
    EXPECT_EQ(user_entity::column<"id">().with_alias("u").alias(), std::string_view("u"));
    EXPECT_EQ(user_entity::column<"id">().table_name(), std::string_view("users"));

    db_predicate dynamic_where;
    EXPECT_TRUE(dynamic_where.empty());
    dynamic_where.and_where(user_entity::column<"department_id">("u") == dept_entity::column<"id">("d")).and_where(user_entity::column<"revision">("u").is_distinct_from(0)).and_where(user_entity::column<"nickname">("u").not_like("tmp%")).and_where(user_entity::column<"nickname">("u").not_ilike("bot%")).and_where(user_entity::column<"nickname">("u").regex("^[a-z]+$")).and_where(user_entity::column<"nickname">("u").iregex("^[a-z0-9]+$")).and_where(user_entity::column<"revision">("u").not_between(10, 20)).and_where(user_entity::column<"revision">("u").not_in({11, 12})).and_where(user_entity::column<"active">("u").is_true()).and_where(user_entity::column<"config">("u").json_contains("{\"enabled\":true}")).and_where(user_entity::column<"config">("u").json_has_key("mode")).and_where(user_entity::column<"config">("u").json_has_any_key({"a", "b"})).and_where(user_entity::column<"config">("u").json_has_all_keys({"c", "d"})).and_where(user_entity::column<"config">("u").json_text_equals("env", "prod")).and_where(user_entity::column<"config">("u").json_path_text_equals({"limits", "tier"}, "gold"));

    db_query sub;
    sub.select(db_expr::column("id")).from("departments");
    dynamic_where.or_where(user_entity::column<"department_id">("u").in(sub) && db_predicate::exists(sub) && db_predicate::not_exists(sub));

    db_query query;
    query
        .select({
            user_entity::column<"id">("u").select(),
            dept_entity::column<"name">("d").as("dept_name"),
            user_entity::column<"nickname">("u").coalesce("anon"),
            user_entity::column<"department_id">("u").coalesce_text(""),
        })
        .from("users", "u")
        .where(dynamic_where.expression(query));
    const auto compiled = query.compile(db_driver::postgresql);
    EXPECT_TRUE(compiled.sql().find("(\"u\".\"department_id\" = \"d\".\"id\")") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("(\"u\".\"revision\" IS DISTINCT FROM $3)") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("NOT LIKE") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("NOT ILIKE") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find(" ~ ") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find(" ~* ") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("NOT BETWEEN") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("NOT IN") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("(\"u\".\"active\" IS TRUE)") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("@> CAST(") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find(" ? ") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find(" ?| CAST(ARRAY[") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find(" ?& CAST(ARRAY[") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("((\"u\".\"config\" ->> ") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("((\"u\".\"config\" #>> CAST(ARRAY[") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("COALESCE(CAST(\"u\".\"department_id\" AS TEXT), ") != std::string_view::npos);

    std::vector<db_assignment> assignments({
        user_entity::column<"nickname">() = "Ada",
        user_entity::column<"department_id">().set_null_if_empty(""),
        user_entity::column<"revision">() += 1,
        user_entity::column<"updated_at">().set_now(),
    });
    const auto copy_of_assignments = decltype(assignments)(assignments);
    db_query update_query;
    update_query.update("users").set(copy_of_assignments).where(user_entity::column<"id">());
    const auto update_compiled = update_query.compile(db_driver::postgresql);
    EXPECT_EQ(update_compiled.sql(),
        "UPDATE \"users\" SET \"nickname\" = $1, \"department_id\" = CAST(NULLIF($2::text, $3::text) AS UUID), "
        "\"revision\" = (\"users\".\"revision\" + $4), \"updated_at\" = \"now\"() WHERE \"users\".\"id\"");
}

RUVIA_DB_ENTITY(alert_entity, "alert_state", db_column<"device_id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>, db_column<"observed_at_ms", std::int64_t>, db_column<"message_id", std::string, db_column_options{.data_type_ = db_data_type::uuid}>, db_column<"data", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>, db_column<"previous_data", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>, db_column<"tags", std::vector<std::string>>)

TEST(Ruvia, db_expr_and_db_tuple_compose_without_external_query) {
    const auto newer_tuple = db_tuple{alert_entity::column<"observed_at_ms">().excluded(), alert_entity::column<"message_id">().excluded()} > db_tuple{alert_entity::column<"observed_at_ms">(), alert_entity::column<"message_id">()};
    const auto prev_expr = db_expr::case_when({{newer_tuple, alert_entity::column<"data">()}}, alert_entity::column<"previous_data">());
    const auto json_patched = db_expr::jsonb_set(alert_entity::column<"data">(), {"meta", "updated"}, db_expr::jsonb_build_object({db_expr::value("at"), db_expr::now(), db_expr::value("first_tag"), alert_entity::column<"tags">().to_expr()[1]}));
    const auto filtered_agg = db_expr::jsonb_agg(alert_entity::column<"device_id">().to_expr().cast(db_data_type::text), {alert_entity::column<"observed_at_ms">().to_expr().desc()}).filter(alert_entity::column<"observed_at_ms">() > 0);
    const db_expr window_partitions[]{alert_entity::column<"device_id">()};
    const db_order_term window_order[]{alert_entity::column<"observed_at_ms">().to_expr().asc()};
    const auto windowed = db_expr::call("lag", {alert_entity::column<"observed_at_ms">()})
                              .over({
                                  .partition_by_ = window_partitions,
                                  .order_by_ = window_order,
                              });
    const auto bracket_where = ((alert_entity::column<"observed_at_ms">() > 100) && ((alert_entity::column<"device_id">() == "a") || (alert_entity::column<"device_id">() == "b"))) &&
        db_tuple{alert_entity::column<"device_id">(), alert_entity::column<"message_id">()}.in({
            db_tuple{db_expr::value("a"), db_expr::value("m1")},
            db_tuple{db_expr::value("b"), db_expr::value("m2")},
        });

    db_query query;
    query
        .select({
            prev_expr.as("prev"),
            json_patched.as("patched"),
            filtered_agg.as("devices"),
            windowed.as("prev_ms"),
        })
        .from("alert_state")
        .where(bracket_where.expression(query));
    const auto compiled = query.compile(db_driver::postgresql);
    EXPECT_TRUE(compiled.sql().find("CASE WHEN (ROW(excluded.\"observed_at_ms\", excluded.\"message_id\") > ROW(\"alert_state\".\"observed_at_ms\", \"alert_state\".\"message_id\")) THEN \"alert_state\".\"data\" ELSE \"alert_state\".\"previous_data\" END AS \"prev\"") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("\"jsonb_set\"(\"alert_state\".\"data\", CAST(ARRAY[") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("(\"alert_state\".\"tags\")[") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("\"jsonb_agg\"(CAST(\"alert_state\".\"device_id\" AS TEXT) ORDER BY \"alert_state\".\"observed_at_ms\" DESC) FILTER (WHERE (\"alert_state\".\"observed_at_ms\" > ") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("\"lag\"(\"alert_state\".\"observed_at_ms\") OVER (PARTITION BY \"alert_state\".\"device_id\" ORDER BY \"alert_state\".\"observed_at_ms\" ASC) AS \"prev_ms\"") != std::string_view::npos);
    EXPECT_TRUE(compiled.sql().find("ROW(\"alert_state\".\"device_id\", \"alert_state\".\"message_id\") IN (ROW(") != std::string_view::npos);
}

TEST(Ruvia, db_expr_from_expression_outlives_source_query_and_maps_table_qualifier) {
    db_expr owned_expr;
    db_expr aliased_expr;
    db_predicate owned_pred;
    {
        {
            db_query source;
            const auto col = db_expr::column("nickname", "users").expression(source);
            const auto pred = db_expr::binary(col, db_binary_operator::equal, db_expr::value(std::string(200, 'a'))).expression(source);
            owned_expr = db_expr::from_expression(col, std::string("users"), false);
            aliased_expr = db_expr::from_expression(db_expr::column("nickname", "u").expression(source), std::string("u"), true);
            owned_pred = db_predicate::from_expression(pred);
        }

        db_query target;
        target
            .select({
                db_expr(owned_expr.expression(target, {}, "u")),
                db_expr(aliased_expr.expression(target, "u", "ignored")),
            })
            .from("users", "u")
            .where(owned_pred.expression(target, "users", "u"));
        const auto compiled = target.compile(db_driver::postgresql);
        EXPECT_EQ(compiled.sql(), R"(SELECT "u"."nickname", "u"."nickname" FROM "users" AS "u" WHERE ("u"."nickname" = $1))");
        EXPECT_EQ(compiled.params().size(), std::size_t{1});

        const auto combined = owned_expr.concat("_suffix");
        db_query combined_target;
        combined_target.select(db_expr(combined.expression(combined_target, {}, "u"))).from("users", "u");
        const auto combined_compiled = combined_target.compile(db_driver::postgresql);
        EXPECT_EQ(combined_compiled.sql(), R"(SELECT ("u"."nickname" || $1) FROM "users" AS "u")");

        owned_expr = {};
        aliased_expr = {};
        owned_pred = {};
    }
}

TEST(Ruvia, db_query_self_import_and_tuple_empty_in_and_predicate_self_merge) {
    {
        db_query query;
        const auto a = db_expr::column("id", "src");
        const auto b = db_expr::column("id", "other");
        const auto expression = db_expr::binary(a, db_binary_operator::equal, b).expression(query);
        (void)db_expr::value(0).expression(query);
        const auto imported = db_expr(expression).expression(query, "src", "dst");
        query.select(db_expr(imported)).from("items", "dst");
        const auto compiled = query.compile(db_driver::postgresql);
        EXPECT_EQ(compiled.sql(), R"(SELECT ("dst"."id" = "other"."id") FROM "items" AS "dst")");
    }
    {
        db_tuple tuple{db_expr::value(1)};
        const auto empty_in = tuple.in({});
        const auto empty_not_in = tuple.not_in({});
        db_query q1;
        q1.select(db_expr::value(1)).where(empty_in.expression(q1));
        const auto c1 = q1.compile(db_driver::postgresql);
        EXPECT_EQ(c1.sql(), "SELECT $1 WHERE FALSE");
        db_query q2;
        q2.select(db_expr::value(1)).where(empty_not_in.expression(q2));
        const auto c2 = q2.compile(db_driver::postgresql);
        EXPECT_EQ(c2.sql(), "SELECT $1 WHERE TRUE");
    }
    {
        auto pred = db_expr::column("id") == 1;
        pred.and_where(pred);
        pred.or_where(pred);
        db_query query;
        query.select(db_expr::value(1)).from("items").where(pred.expression(query));
        const auto compiled = query.compile(db_driver::postgresql);
        EXPECT_EQ(compiled.sql(), R"(SELECT $1 FROM "items" WHERE ((("id" = $2) AND ("id" = $3)) OR (("id" = $4) AND ("id" = $5))))");
    }
}

RUVIA_DB_PROJECTION(projection_schema_check, db_column<"id", std::int64_t>, db_column<"label", std::string>)

TEST(Ruvia, db_query_clear_and_bitwise_and_projection_schema) {
    static_assert(projection_schema_check::column_index<"id">() == 0);
    static_assert(projection_schema_check::column_index<"label">() == 1);
    static_assert(projection_schema_check::column_name<"id">() == "id");
    static_assert(projection_schema_check::column_name<"label">() == "label");
    const auto bit_expr = ((db_expr::column("flags") & 3) | 4) ^ 1;
    db_query query;
    query.select(bit_expr).from("items").where(db_expr::binary(db_expr::column("id"), db_binary_operator::equal, db_expr::value(1))).group_by({db_expr::column("id")}).having(db_expr::binary(db_expr::column("id"), db_binary_operator::greater, db_expr::value(0)));
    EXPECT_TRUE(query.has_where());
    query.clear_where().clear_having();
    EXPECT_TRUE(!query.has_where());
    auto compiled = query.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), R"(SELECT ((("flags" & $1) | $2) # $3) FROM "items" GROUP BY "id")");

    db_query other;
    other.select(db_expr::value(42));
    compiled = other.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), "SELECT $1");
    EXPECT_EQ(compiled.params().size(), std::size_t{1});
}

TEST(Ruvia, db_query_postgresql_numbers_each_parameter_node_once_and_caps_the_count) {
    // Equal grouped expressions imported separately into SELECT and GROUP BY
    // share their numbers, beyond any small fixed table of reused nodes.
    db_query grouped;
    std::vector<db_selection> projections{};
    std::vector<db_expr> groups{};
    for (int bucket = 0; bucket < 40; ++bucket) {
        const auto expression = db_expr::column("id") + bucket;
        projections.push_back(expression);
        groups.push_back(expression);
    }
    grouped.select(projections).from("events").group_by(groups);
    const auto statement = grouped.compile(db_driver::postgresql);
    EXPECT_EQ(statement.params().size(), std::size_t{40});
    EXPECT_TRUE(statement.sql().find("FROM \"events\" GROUP BY (\"id\" + $1), (\"id\" + $2), ") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().ends_with("(\"id\" + $40)"));

    // The PostgreSQL Bind message counts parameters in an Int16.
    db_query bulk;
    bulk.insert_into("events", {"id"});
    std::vector<db_expr> values{};
    values.reserve(65536);
    for (std::int64_t id = 0; id < 65536; ++id) {
        values.push_back(db_expr::value(id));
    }
    const std::string_view columns[]{"id"};
    const std::span<const db_expr> all_values(values);
    bulk.insert_rows(columns, all_values.first(65535), 65535);
    const auto bound = bulk.compile(db_driver::postgresql);
    EXPECT_EQ(bound.params().size(), std::size_t{65535});
    bulk.insert_rows(columns, all_values.last(1), 1);
    bool capped = false;
    try {
        (void)bulk.compile(db_driver::postgresql);
    } catch (const std::length_error&) {
        capped = true;
    }
    EXPECT_TRUE(capped);
    const auto literal = bulk.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_TRUE(literal.params().empty());
}

TEST(Ruvia, db_expr_stored_table_never_rebinds_a_foreign_table_to_the_caller_alias) {
    db_query source;
    const auto tenant = db_expr::binary(db_expr::column("tenant_id", "orders"), op_type::equal, db_expr::value(7)).expression(source);
    const auto owned = db_expr::from_expression(tenant, "orders");
    const db_expr borrowed(tenant, "orders");
    for (const auto* expression : {&owned, &borrowed}) {
        db_query users;
        users.select(db_selection::star("u")).from("users", "u").where(expression->expression(users, "users", "u"));
        EXPECT_EQ(compile_sql([&] {
            return users.compile(db_driver::postgresql);
        }),
            R"(SELECT "u".* FROM "users" AS "u" WHERE ("orders"."tenant_id" = $1))");
        // Without a caller table, the stored table names the source.
        db_query orders;
        orders.select(db_selection::star("o")).from("orders", "o").where(expression->expression(orders, {}, "o"));
        EXPECT_EQ(compile_sql([&] {
            return orders.compile(db_driver::postgresql);
        }),
            R"(SELECT "o".* FROM "orders" AS "o" WHERE ("o"."tenant_id" = $1))");
    }
}

TEST(Ruvia, db_query_statement_kind_change_rejects_clauses_that_would_widen_the_target) {
    db_query query;
    query.from("users", "u").where(db_expr::binary(db_expr::column("id", "u"), op_type::equal, db_expr::value(1)));
    EXPECT_TRUE(testing::throws_on([&] {
        query.delete_from("users");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.update("users");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.insert_into("users");
    }));
    EXPECT_EQ(compile_sql([&] {
        return query.compile(db_driver::postgresql);
    }),
        R"(SELECT * FROM "users" AS "u" WHERE ("u"."id" = $1))");

    db_query projected;
    projected.select(db_expr::value(1));
    EXPECT_TRUE(testing::throws_on([&] {
        projected.update("users");
    }));

    // WHERE carries over; re-targeting the same kind and the explicit USING
    // spelling stay available.
    db_query filtered;
    filtered.where(db_expr::binary(db_expr::column("id", "a"), op_type::equal, db_expr::value(1))).delete_from("users");
    filtered.delete_from("accounts", "a").delete_using("users", "u");
    EXPECT_EQ(compile_sql([&] {
        return filtered.compile(db_driver::postgresql);
    }),
        R"(DELETE FROM "accounts" AS "a" USING "users" AS "u" WHERE ("a"."id" = $1))");
    EXPECT_TRUE(testing::throws_on([&] {
        filtered.update("accounts");
    }));
}

static_assert(!std::is_constructible_v<db_expr, db_expression, std::string>);
static_assert(std::is_constructible_v<db_expr, db_expression, const std::string&>);
static_assert(std::is_constructible_v<db_expr, db_expression, std::string_view>);

TEST(Ruvia, db_query_raw_sql_keeps_comment_tokens_and_positional_parameters_out) {
    const auto difference = db_expr::raw_sql({"(10 -", ")"}, {db_expr::value(-5)});
    const auto quotient = db_expr::raw_sql({"10 /", ""}, {db_expr::star()});
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        EXPECT_EQ(db_query::render_expression(difference, driver), "((10 - -5))");
        EXPECT_EQ(db_query::render_expression(quotient, driver), "(10 / *)");
    }

    db_query positional;
    positional.select(db_expr::raw_sql({"$1 + ", ""}, {db_expr::value(1)}));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)positional.compile(db_driver::postgresql);
    }));
    db_query trailing;
    trailing.select(db_expr::raw_sql({"", " = $2"}, {db_expr::value(1)}));
    EXPECT_TRUE(testing::throws_on([&] {
        (void)trailing.compile(db_driver::postgresql);
    }));
    // Literal output (SQL function bodies) and MariaDB do not number `$n`.
    EXPECT_EQ(compile_sql([&] {
        return positional.compile(db_driver::postgresql, db_parameter_mode::literal);
    }),
        "SELECT ($1 + 1)");
    EXPECT_EQ(compile_sql([&] {
        return positional.compile(db_driver::mariadb);
    }),
        "SELECT ($1 + ?)");
    // A `$` that continues an identifier is not a parameter.
    db_query identifier;
    identifier.select(db_expr::raw_sql("price$1")).from("items");
    EXPECT_EQ(compile_sql([&] {
        return identifier.compile(db_driver::postgresql);
    }),
        R"(SELECT (price$1) FROM "items")");
}

TEST(Ruvia, db_query_rejects_dotted_aliases_cte_names_and_lock_targets) {
    db_query query;
    db_query body;
    body.select(db_expr::value(1));
    const borrowed_text dotted_tables[]{"u.v"};
    EXPECT_TRUE(testing::throws_on([&] {
        query.from("users", "u.v");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.from(body, "s.t");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.join(db_join_type::cross, "accounts", {}, "a.b");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.join(db_join_type::cross, body, {}, "a.b");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.with("w.x", body);
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        query.lock({.tables_ = dotted_tables});
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        db_query().update("users", "u.v");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        db_query().delete_from("users", "u.v");
    }));
    EXPECT_TRUE(testing::throws_on([&] {
        db_query().insert_into("users", {}, "u.v");
    }));

    // Schema-qualified tables keep working with single-identifier aliases.
    const borrowed_text lock_tables[]{"u"};
    query.from("app.users", "u").lock({.tables_ = lock_tables});
    EXPECT_EQ(compile_sql([&] {
        return query.compile(db_driver::postgresql);
    }),
        R"(SELECT * FROM "app"."users" AS "u" FOR UPDATE OF "u")");
}

TEST(Ruvia, db_query_and_or_chains_are_one_nesting_level) {
    // 300 conjuncts would exceed the 256-level limit if every AND nested.
    db_predicate filter;
    db_query built;
    db_query correlated;
    correlated.select(db_expr::value(1)).from("events", "e");
    for (int index = 0; index < 300; ++index) {
        filter.and_where(db_expr::column("id") != index);
        built.and_where(db_expr::binary(db_expr::column("id"), op_type::not_equal, db_expr::value(index)));
        correlated.and_where(db_expr::binary(db_expr::column("owner", "src"), op_type::not_equal, db_expr::value(index)));
    }
    db_query owned;
    owned.select(db_selection::star()).from("items").where(filter.expression(owned));
    const auto owned_statement = owned.compile(db_driver::postgresql);
    EXPECT_EQ(owned_statement.params().size(), std::size_t{300});
    EXPECT_TRUE(owned_statement.sql().starts_with(R"(SELECT * FROM "items" WHERE (("id" <> $1) AND ("id" <> $2) AND ("id" <> $3) AND )"));
    EXPECT_TRUE(owned_statement.sql().ends_with(R"( AND ("id" <> $300)))"));

    built.select(db_selection::star()).from("items");
    EXPECT_EQ(compile_sql([&] {
        return built.clone().compile(db_driver::postgresql);
    }),
        std::string(owned_statement.sql()));

    // Importing with a qualifier mapping walks the chain inside a correlated subquery too.
    const auto exists = db_predicate::exists(correlated);
    db_query target;
    target.select(db_selection::star()).from("items", "dst").where(exists.expression(target, "src", "dst"));
    const auto target_sql = compile_sql([&] {
        return target.compile(db_driver::postgresql);
    });
    EXPECT_TRUE(target_sql.find(R"(WHERE EXISTS (SELECT $1 FROM "events" AS "e" WHERE (("dst"."owner" <> $2) AND ("dst"."owner" <> $3) AND )") != std::string::npos);
    EXPECT_TRUE(target_sql.find(R"("src")") == std::string::npos);
}

TEST(Ruvia, db_query_rvalue_composition_matches_copying_and_consumes_the_argument) {
    const auto make_source = [] {
        db_query source;
        source.select(db_expr::value(1)).from("events");
        return source;
    };
    db_query copied;
    const auto copied_source = make_source();
    copied.select(db_selection::star()).from(copied_source, "s").where(db_predicate::exists(copied_source));
    const auto expected = compile_sql([&] {
        return copied.compile(db_driver::postgresql);
    });

    db_query moved;
    auto adopted = make_source();
    moved.select(db_selection::star()).from(std::move(adopted), "s").where(db_predicate::exists(make_source()));
    EXPECT_EQ(compile_sql([&] {
        return moved.compile(db_driver::postgresql);
    }),
        expected);
    EXPECT_TRUE(testing::throws_on([&] {
        (void)adopted.compile(db_driver::postgresql);
    }));
}

enum class query_composition { scalar,
    exists,
    source,
    join,
    insert,
    cte,
    combine };

db_query composition_target(query_composition operation) {
    db_query target;
    if (operation == query_composition::insert) {
        db_query initial;
        initial.select(db_expr::value(7));
        target.insert_into("items", {"id"}).insert_from(std::move(initial));
    } else {
        target.select(db_expr::value(9)).from("items");
    }
    return target;
}

template <typename query_type>
std::optional<db_expr> compose_query(db_query& target, query_type&& source, query_composition operation) {
    constexpr std::string_view alias = "nested_query_with_a_long_alias";
    constexpr std::array<borrowed_text, 1> columns{"nested_column_with_a_long_name"};
    switch (operation) {
        case query_composition::scalar:
            return db_expr::subquery(source);
        case query_composition::exists:
            return db_expr(db_predicate::exists(source));
        case query_composition::source:
            target.from(std::forward<query_type>(source), alias);
            break;
        case query_composition::join:
            target.join(db_join_type::cross, std::forward<query_type>(source), {}, alias);
            break;
        case query_composition::insert:
            target.insert_from(std::forward<query_type>(source));
            break;
        case query_composition::cte:
            target.with(alias, std::forward<query_type>(source), {.columns_ = columns});
            break;
        case query_composition::combine:
            target.combine(db_set_operation::union_all, std::forward<query_type>(source));
            break;
    }
    return std::nullopt;
}

TEST(Ruvia, db_query_rvalue_and_self_composition_preserve_snapshots) {
    for (const auto operation : {query_composition::scalar, query_composition::exists, query_composition::source, query_composition::join, query_composition::insert, query_composition::cte, query_composition::combine}) {
        SCOPED_TRACE(static_cast<int>(operation));
        auto target = composition_target(operation);
        db_query source;
        source.select(db_expr::value(17)).from("source_rows");
        auto expected = target.clone();
        if (const auto expression = compose_query(expected, std::as_const(source), operation)) {
            expected.select(*expression);
        }
        const auto expected_sql = compile_sql([&] {
            return expected.compile(db_driver::postgresql, db_parameter_mode::literal);
        });
        if (const auto expression = compose_query(target, std::move(source), operation)) {
            target.select(*expression);
        }
        // Expression factories copy their query; only clause entry points take it over.
        if (operation != query_composition::scalar && operation != query_composition::exists) {
            EXPECT_THROW((void)source.returns_rows(), std::logic_error);
        }
        EXPECT_EQ(compile_sql([&] {
            return target.compile(db_driver::postgresql, db_parameter_mode::literal);
        }),
            expected_sql);
    }
    for (const auto operation : {query_composition::scalar, query_composition::exists, query_composition::source, query_composition::join, query_composition::cte, query_composition::combine}) {
        SCOPED_TRACE(static_cast<int>(operation));
        auto query = composition_target(operation);
        auto expected = query.clone();
        if (const auto expression = compose_query(expected, std::as_const(query), operation)) {
            expected.select(*expression);
        }
        const auto expected_sql = compile_sql([&] {
            return expected.compile(db_driver::postgresql, db_parameter_mode::literal);
        });
        if (const auto expression = compose_query(query, std::move(query), operation)) {
            query.select(*expression);
        }
        EXPECT_EQ(compile_sql([&] {
            return query.compile(db_driver::postgresql, db_parameter_mode::literal);
        }),
            expected_sql);
    }
}

TEST(Ruvia, db_query_limit_and_offset_above_int64_max_throw_on_postgresql_and_compile_unsigned_on_mariadb) {
    constexpr auto int64_max = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    constexpr auto above_int64_max = int64_max + 1ULL;
    constexpr auto uint64_max = (std::numeric_limits<std::uint64_t>::max)();

    // INT64_MAX boundary succeeds on both PostgreSQL and MariaDB.
    {
        db_query boundary;
        boundary.from("device").limit(int64_max).offset(int64_max);
        const auto pg = boundary.compile(db_driver::postgresql);
        EXPECT_EQ(pg.sql(), "SELECT * FROM \"device\" LIMIT $1 OFFSET $2");
        ASSERT_EQ(pg.params().size(), std::size_t{2});
        EXPECT_EQ(pg.params()[0].type(), db_value_type::signed_value);
        EXPECT_EQ(pg.params()[0].signed_value(), (std::numeric_limits<std::int64_t>::max)());
        EXPECT_EQ(pg.params()[1].signed_value(), (std::numeric_limits<std::int64_t>::max)());
    }

    // Values above INT64_MAX throw std::invalid_argument on PostgreSQL (both bound and literal),
    // and compile unsigned without negative wraparound on MariaDB.
    for (const std::uint64_t value : {static_cast<std::uint64_t>(above_int64_max), static_cast<std::uint64_t>(uint64_max)}) {
        db_query limit_only;
        limit_only.from("device").limit(value);
        EXPECT_THROW((void)limit_only.compile(db_driver::postgresql), std::invalid_argument);
        EXPECT_THROW((void)limit_only.compile(db_driver::postgresql, db_parameter_mode::literal), std::invalid_argument);

        db_query offset_only;
        offset_only.from("device").offset(value);
        EXPECT_THROW((void)offset_only.compile(db_driver::postgresql), std::invalid_argument);
        EXPECT_THROW((void)offset_only.compile(db_driver::postgresql, db_parameter_mode::literal), std::invalid_argument);

        const auto maria_limit_bound = limit_only.compile(db_driver::mariadb);
        EXPECT_EQ(maria_limit_bound.sql(), "SELECT * FROM `device` LIMIT ?");
        ASSERT_EQ(maria_limit_bound.params().size(), std::size_t{1});
        EXPECT_EQ(maria_limit_bound.params()[0].type(), db_value_type::unsigned_value);
        EXPECT_EQ(maria_limit_bound.params()[0].unsigned_value(), value);

        const auto maria_offset_bound = offset_only.compile(db_driver::mariadb);
        EXPECT_EQ(maria_offset_bound.sql(), "SELECT * FROM `device` LIMIT 18446744073709551615 OFFSET ?");
        ASSERT_EQ(maria_offset_bound.params().size(), std::size_t{1});
        EXPECT_EQ(maria_offset_bound.params()[0].type(), db_value_type::unsigned_value);
        EXPECT_EQ(maria_offset_bound.params()[0].unsigned_value(), value);
    }

    db_query both;
    both.from("device").limit(above_int64_max).offset(uint64_max);
    const auto maria_literal = both.compile(db_driver::mariadb, db_parameter_mode::literal);
    EXPECT_EQ(maria_literal.sql(), "SELECT * FROM `device` LIMIT 9223372036854775808 OFFSET 18446744073709551615");
    EXPECT_EQ(maria_literal.sql().find('-'), std::string_view::npos);
}

}  // namespace
