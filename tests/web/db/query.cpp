#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/redis/redis.h"

#include "backend_client_fixture.h"
#include "test_exceptions.h"
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
        query.select({query.value(text), query.value(-42),
            query.value((std::numeric_limits<std::uint64_t>::max)()), query.value(1.25), query.value(true)});
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
    EXPECT_EQ(parameters[3].type(), db_value_type::double_value);
    EXPECT_EQ(parameters[3].double_value(), 1.25);
    EXPECT_EQ(parameters[4].type(), db_value_type::bool_value);
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
            db_query source_value;
            auto expression = source_value.sql({"jsonb_set(", ", '{label}', to_jsonb(", "::text))"},
                {source_value.column("payload"), source_value.value("x'); DELETE FROM device; --")});
            target.update("device").set("payload", target.import_expression(expression));
            target.returning({target.column("id")});
        }
        auto statement = target.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), "UPDATE \"device\" SET \"payload\" = (jsonb_set(\"payload\", '{label}', to_jsonb($1::text))) RETURNING \"id\"");
        EXPECT_EQ(statement.params().size(), std::size_t{1});
        EXPECT_TRUE(testing::throws_on([&] { (void)target.sql({"one"}, {target.value(1)}); }));
        EXPECT_TRUE(testing::throws_on([&] { (void)target.sql(std::string_view("x\0y", 3)); }));
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
        query.update("accounts").set("balance", query.value(100));
        query.where(query.binary(query.column("id"), op_type::equal, query.value(7)));
        const auto original = compile_sql([&] { return query.compile(driver, db_parameter_mode::literal); });
        bool rejected = false;
        try {
            query.set({{"balance", db_expr::value(0)}, {"", db_expr::value(1)}});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(compile_sql([&] { return query.compile(driver, db_parameter_mode::literal); }), original);
        query.set({{"balance", db_expr::value(1)}, {"revision", db_expr::value(2)}, {"balance", db_expr::value(3)}});
        const auto committed = query.compile(driver, db_parameter_mode::literal);
        EXPECT_EQ(committed.sql(), driver == db_driver::postgresql
                                       ? "UPDATE \"accounts\" SET \"balance\" = 3, \"revision\" = 2 WHERE (\"id\" = 7)"
                                       : "UPDATE `accounts` SET `balance` = 3, `revision` = 2 WHERE (`id` = 7)");
    }
}

TEST(Ruvia, db_query_insert_target_cannot_reinterpret_rows_and_rejected_batches_preserve_them) {
    db_query query;
    query.insert_into("pairs", {"a"}).values({query.value(11)});
    const auto original = compile_sql([&] { return query.compile(db_driver::postgresql, db_parameter_mode::literal); });
    EXPECT_TRUE(testing::throws_on([&] { query.insert_into("pairs", {"b"}); }));
    const std::array<std::string_view, 1> columns{"b"};
    const std::array values{query.value(22)};
    EXPECT_TRUE(testing::throws_on([&] { query.insert_rows(columns, values, 2); }));
    db_query source;
    source.select(source.value(9));
    EXPECT_TRUE(testing::throws_on([&] { query.insert_from(source); }));
    EXPECT_EQ(compile_sql([&] { return query.compile(db_driver::postgresql, db_parameter_mode::literal); }), original);
    query.insert_rows(columns, values, 1);
    const auto committed = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(committed.sql(), "INSERT INTO \"pairs\" (\"a\", \"b\") VALUES (11, DEFAULT), (DEFAULT, 22)");
}

TEST(Ruvia, db_query_reuses_postgresql_parameters_for_grouped_expressions) {
    db_query query;
    const auto bucket = query.binary(query.column("id"), op_type::add, query.value(1));
    query.select({bucket, query.aggregate("count", {query.star()})}).from("events").group_by({bucket});
    const auto postgres = query.compile(db_driver::postgresql);
    EXPECT_EQ(postgres.sql(), "SELECT (\"id\" + $1), \"count\"(*) FROM \"events\" GROUP BY (\"id\" + $1)");
    EXPECT_EQ(postgres.params().size(), std::size_t{1});
    const auto maria = query.compile(db_driver::mariadb);
    EXPECT_EQ(maria.params().size(), std::size_t{2});
}

TEST(Ruvia, db_query_binds_values_in_wire_order_and_preserves_zero_limit) {
    db_query query;
    const auto second = query.value(db_value("O'Reilly $1 ?"));
    const auto first = query.value(db_value(9));
    const auto id = query.column("id", "u");
    query.select(first).from("users", "u").where(query.binary(id, op_type::equal, second));
    query.order_by(id, db_order_direction::desc).limit(0).lock({.nowait_ = true});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT $1 FROM \"users\" AS \"u\" WHERE (\"u\".\"id\" = $2) ORDER BY \"u\".\"id\" DESC LIMIT $3 FOR UPDATE NOWAIT");
    EXPECT_EQ(statement.params().size(), std::size_t{3});
}

TEST(Ruvia, db_query_nested_ctes_are_owned_and_use_one_parameter_sequence) {
    db_query query;
    {
        db_query incoming;
        const std::array row{incoming.value(db_value(7)), incoming.value(db_value("first"))};
        incoming.values(row);
        std::string column_name("name");
        const borrowed_text columns[]{"id", column_name};
        const db_cte_options options{.materialization_ = db_materialization::materialized, .columns_ = columns};
        query.with("incoming", incoming, options);
        column_name.assign("gone");
        incoming.values(row);
    }
    db_query filter;
    filter.select(filter.column("id")).from("incoming").where(filter.binary(filter.column("name"), op_type::equal, filter.value(db_value("first"))));
    query.select(query.value(db_value(11))).from(filter, "filtered");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "WITH \"incoming\" (\"id\", \"name\") AS MATERIALIZED (VALUES ($1, $2)) SELECT $3 FROM (SELECT \"id\" FROM \"incoming\" WHERE (\"name\" = $4)) AS \"filtered\"");
    EXPECT_EQ(statement.params().size(), std::size_t{4});
}

TEST(Ruvia, db_query_postgresql_casts_bound_values_only_where_nothing_types_them) {
    db_query query;
    const auto name = query.column("name");
    const auto payload = query.column("payload");
    const std::array<db_expression, 20> projections{
        // "any" arguments give an untyped parameter nothing to resolve against.
        query.call("json_build_object", {query.value("k"), query.value(5), query.value("on"), query.value(true)}),
        query.call("concat", {name, query.value("-"), query.value(1.5)}),
        // int4-only signatures are typed int4 explicitly.
        query.call("left", {name, query.value(3)}),
        query.call("pg_catalog.round", {query.column("price"), query.value(2)}),
        // Other arguments keep the type of their context (uuid, json, timestamp...).
        query.call("date_trunc", {query.value("day"), query.column("created_at")}),
        // Arithmetic over two bound operands selects no overload; one typed
        // operand types the other. unknown || unknown resolves to text.
        query.binary(query.value("a"), op_type::concat, query.value("b")),
        query.binary(name, op_type::concat, query.value("b")),
        query.binary(query.column("created_on"), op_type::add, query.value(1)),
        query.binary(query.value(2), op_type::multiply, query.value(3)),
        // An integer jsonb subscript is an array index, never a text key.
        query.binary(payload, op_type::json_get, query.value(0)),
        query.binary(payload, op_type::json_get_text, query.value("key")),
        // A typed function overload resolves its untyped argument; negation
        // and GREATEST over bound values have nothing to resolve against.
        query.call("abs", {query.value(-3)}),
        query.unary(db_unary_operator::negate, query.value(4)),
        query.greatest({query.value(1), query.value(2)}),
        // The timestamp overload types the stop and step arguments.
        query.call("generate_series", {query.column("start_at"), query.value("2026-01-02 00:00:00"), query.value("1 day")}),
        // A separator is text; the "any" arguments after it are not.
        query.call("concat_ws", {query.value(","), query.value(1)}),
        // Boolean predicates and logical operators type their operands.
        query.unary(db_unary_operator::is_true, query.value("true")),
        query.binary(query.value(true), op_type::and_value, query.value(false)),
        // The null predicates accept every type, so nothing types the operand.
        query.unary(db_unary_operator::is_null, query.value(7)),
        // Comparing two bound values resolves as text.
        query.binary(query.value("x"), op_type::equal, query.value("y")),
    };
    query.select(projections).from("items").where(query.binary(query.column("ratio"), op_type::greater, query.value(0.25)));
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
    cast.select(cast.cast(cast.value(7), db_type_definition{.custom_name_ = "app_code"})).from("items");
    const auto cast_statement = cast.compile(db_driver::postgresql);
    EXPECT_EQ(cast_statement.sql(), "SELECT CAST($1 AS \"app_code\") FROM \"items\"");
    // Concatenation never forces a category (int8 || int8 does not exist), and
    // a reused string parameter keeps the numeric type its first use gave it.
    db_query reuse;
    const auto amount = reuse.value("1.5");
    const std::array reuse_projections{reuse.binary(reuse.value(1), op_type::concat, reuse.value(2)),
        reuse.cast(amount, db_type_definition{.data_type_ = db_data_type::numeric}),
        reuse.unary(db_unary_operator::negate, amount)};
    reuse.select(reuse_projections).from("items");
    const auto reuse_statement = reuse.compile(db_driver::postgresql);
    EXPECT_EQ(reuse_statement.sql(), "SELECT ($1 || $2), CAST($3 AS DECIMAL), (- $3) FROM \"items\"");
    db_query maria_cast;
    maria_cast.select(maria_cast.cast(maria_cast.value("1.5"), db_type_definition{.data_type_ = db_data_type::numeric}));
    EXPECT_TRUE(testing::throws_on([&] { (void)maria_cast.compile(db_driver::mariadb); }));
    db_query maria_time;
    maria_time.select(maria_time.cast(maria_time.value("2026-01-01 00:00:00.5"), db_type_definition{.data_type_ = db_data_type::timestamp}));
    const auto maria_time_statement = maria_time.compile(db_driver::mariadb);
    EXPECT_EQ(maria_time_statement.sql(), "SELECT CAST(? AS DATETIME(6))");
    // Unsigned values span beyond bigint, so their category is numeric:
    // COALESCE over UINT64_MAX must not narrow it to int8.
    db_query wide;
    wide.select(wide.coalesce({wide.value(std::numeric_limits<std::uint64_t>::max()), wide.value(std::uint64_t{1})}))
        .from("items");
    const auto wide_statement = wide.compile(db_driver::postgresql);
    EXPECT_EQ(wide_statement.sql(), "SELECT COALESCE($1::numeric, $2::numeric) FROM \"items\"");
    EXPECT_EQ(wide_statement.params().size(), std::size_t{2});
    const auto wide_literal = wide.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(wide_literal.sql(), "SELECT COALESCE(18446744073709551615, 1) FROM \"items\"");
    EXPECT_TRUE(wide_literal.params().empty());
    // MariaDB placeholders carry no casts.
    db_query maria;
    maria.select(maria.call("left", {maria.column("name"), maria.value(3)})).from("items").where(maria.binary(maria.column("ratio"), op_type::greater, maria.value(0.25)));
    const auto maria_statement = maria.compile(db_driver::mariadb);
    EXPECT_EQ(maria_statement.sql(), "SELECT left(`name`, ?) FROM `items` WHERE (`ratio` > ?)");
}

TEST(Ruvia, db_query_literal_doubles_keep_the_bound_double_type) {
    // Bound doubles are float8/DOUBLE; a literal 2 would make "qty / 2" integer
    // division and a literal 2.5 is exact NUMERIC/DECIMAL in both dialects.
    db_query query;
    query.select({query.binary(query.column("qty"), op_type::divide, query.value(2.0)),
                     query.value(2.5), query.value(-0.5), query.value(1e20)})
        .from("items");
    const auto postgres = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(postgres.sql(),
        "SELECT (\"qty\" / CAST('2' AS DOUBLE PRECISION)), CAST('2.5' AS DOUBLE PRECISION), "
        "CAST('-0.5' AS DOUBLE PRECISION), CAST('1e+20' AS DOUBLE PRECISION) FROM \"items\"");
    const auto maria = query.compile(db_driver::mariadb, db_parameter_mode::literal);
    EXPECT_EQ(maria.sql(), "SELECT (`qty` / 2e0), 2.5e0, -0.5e0, 1e+20 FROM `items`");
    EXPECT_EQ(db_query::render_expression(query.unary(db_unary_operator::negate, query.value(-2.0)), db_driver::mariadb),
        "(- -2e0)");
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
    converted.select({converted.value(db_value(1.5L)),
        converted.value(db_value(static_cast<long double>(maximum))),
        converted.value(db_value(-static_cast<long double>(maximum)))});
    db_query reference;
    reference.select({reference.value(1.5), reference.value(maximum), reference.value(-maximum)});
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
        const auto operand = query.value(item.value_);
        const auto mask = query.value(std::uint64_t{1});
        const auto inverted = query.unary(db_unary_operator::bit_not, operand);
        query.select({query.binary(operand, op_type::bit_and, mask),
            query.binary(operand, op_type::bit_or, mask),
            query.binary(operand, op_type::bit_xor, mask),
            inverted});
        const auto statement = query.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), "SELECT ($1::int8 & $2::int8), ($1::int8 | $2::int8), ($1::int8 # $2::int8), (~ $1::int8)");
        EXPECT_EQ(statement.params().size(), std::size_t{2});
        const auto literal = query.compile(db_driver::postgresql, db_parameter_mode::literal);
        const auto operand_literal = "(" + std::string(item.literal_) + ")::int8";
        const auto literal_sql = std::string("SELECT (") + operand_literal + std::string(" & (1)::int8), (") + operand_literal +
                                 std::string(" | (1)::int8), (") + operand_literal + std::string(" # (1)::int8), (~ ") + operand_literal + ")";
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
            (void)query.collate(query.column("name"), invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    db_query query;
    const auto collated = query.collate(query.column("name"), "pg_catalog.C");
    EXPECT_EQ(db_query::render_expression(collated, db_driver::postgresql),
        std::string_view(R"(("name" COLLATE "pg_catalog"."C"))"));
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
                    const auto overflow = query.value(value);
                    const auto mask = query.value(std::uint64_t{1});
                    query.select(query.binary(overflow_on_left ? overflow : mask, op, overflow_on_left ? mask : overflow));
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
            query.select(query.unary(db_unary_operator::bit_not, query.value(value)));
            bool rejected = false;
            try {
                (void)query.compile(db_driver::postgresql, mode);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
            db_query reused;
            const auto overflow = reused.value(value);
            reused.select({reused.coalesce({overflow, overflow}), reused.unary(db_unary_operator::bit_not, overflow)});
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

TEST(Ruvia, db_query_postgresql_reuses_one_parameter_across_numeric_and_bitwise_casts) {
    for (const bool numeric_first : {true, false}) {
        db_query query;
        const auto value = query.value(std::uint64_t{7});
        const auto numeric = query.coalesce({value, value});
        const auto binary = query.binary(value, op_type::bit_and, value);
        const auto unary = query.unary(db_unary_operator::bit_not, value);
        query.select(numeric_first ? std::array{numeric, binary, unary} : std::array{binary, unary, numeric});
        const auto statement = query.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(), numeric_first
                                       ? "SELECT COALESCE($1::numeric, $1::numeric), ($1::int8 & $1::int8), (~ $1::int8)"
                                       : "SELECT ($1::int8 & $1::int8), (~ $1::int8), COALESCE($1::numeric, $1::numeric)");
        EXPECT_EQ(statement.params().size(), std::size_t{1});
    }
}

TEST(Ruvia, db_query_postgresql_bitwise_values_keep_column_and_explicit_cast_contexts) {
    db_query query;
    const auto value = query.value(std::numeric_limits<std::uint64_t>::max());
    const auto flags = query.column("flags");
    const auto cast = query.cast(value, db_type_definition{.custom_name_ = "app_flags"});
    query.select({query.binary(flags, op_type::bit_and, value),
                     query.binary(value, op_type::bit_or, flags),
                     query.binary(cast, op_type::bit_xor, value),
                     query.unary(db_unary_operator::bit_not, cast)})
        .from("items");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(),
        "SELECT (\"flags\" & $1), ($1 | \"flags\"), (CAST($1 AS \"app_flags\") # $1), (~ CAST($1 AS \"app_flags\")) FROM \"items\"");
    EXPECT_EQ(statement.params().size(), std::size_t{1});
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
    query.select(query.value(from_mutable)).from("items").where(query.binary(query.column("name"), op_type::equal, query.value(from_const)));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.params().size(), std::size_t{2});
}

TEST(Ruvia, db_query_null_comparisons_and_empty_membership_have_defined_semantics) {
    db_query query;
    const auto id = query.column("id");
    const std::array projections{query.binary(id, op_type::equal, query.null_value()), query.binary(query.null_value(), op_type::not_equal, id),
        query.binary(id, op_type::in, query.list({})), query.binary(id, op_type::not_in, query.list({}))};
    query.select(projections).from("devices");
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT (\"id\" IS NULL), (\"id\" IS NOT NULL), FALSE, TRUE FROM \"devices\"");
    EXPECT_TRUE(statement.params().empty());
}

TEST(Ruvia, db_query_null_comparisons_share_unary_parameter_typing) {
    const auto value = db_expr::value(7);
    const auto null = db_expr::null();
    const std::array predicates{
        value == nullptr, null == value, value != nullptr, null != value,
        value.is_null(), value.is_not_null()};
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        for (const auto mode : {db_parameter_mode::bound, db_parameter_mode::literal}) {
            for (std::size_t i = 0; i < predicates.size(); ++i) {
                const bool negate = i == 2 || i == 3 || i == 5;
                const auto op = negate ? db_binary_operator::not_equal : db_binary_operator::equal;
                db_query expected;
                expected.select(expected.unary(negate ? db_unary_operator::is_not_null : db_unary_operator::is_null,
                    expected.value(7)));
                const auto reference = expected.compile(driver, mode);
                db_query owned;
                owned.select(predicates[i].expression(owned));
                const auto owned_statement = owned.compile(driver, mode);
                EXPECT_EQ(owned_statement.sql(), reference.sql());
                EXPECT_EQ(owned_statement.params().size(), reference.params().size());
                db_query direct;
                const auto operand = direct.value(7);
                const auto null_value = direct.null_value();
                direct.select(i == 1 || i == 3
                                  ? direct.binary(null_value, op, operand)
                                  : direct.binary(operand, op, null_value));
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
    auto ts = query.column("ts", "e");
    auto id = query.column("id", "e");
    const db_expr partitions[]{id};
    const db_order_term window_order[]{{ts, db_order_direction::desc}};
    auto rank = query.over(query.call("row_number"), {.partition_by_ = partitions, .order_by_ = window_order});
    const std::array args{id};
    const std::array order{db_order_term{ts, db_order_direction::asc}};
    auto aggregate = query.filter(query.aggregate("array_agg", args, false, order), query.unary(db_unary_operator::is_not_null, id));
    const std::array projections{query.alias(rank, "rank"), aggregate};
    query.select(projections).from("events", "e");
    query.join_function(db_join_type::cross, query.call("jsonb_each", {query.column("data", "e")}), {}, "kv", {.lateral_ = true});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_TRUE(statement.sql().find("\"row_number\"() OVER (PARTITION BY \"e\".\"id\" ORDER BY \"e\".\"ts\" DESC)") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("\"array_agg\"(\"e\".\"id\" ORDER BY \"e\".\"ts\" ASC) FILTER (WHERE (\"e\".\"id\" IS NOT NULL))") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("CROSS JOIN LATERAL \"jsonb_each\"(\"e\".\"data\") AS \"kv\"") != std::string_view::npos);
}

TEST(Ruvia, db_query_expression_helpers_render_postgresql_and_reject_empty_inputs) {
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
    const std::array<db_expression, 12> projections{
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

    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(),
        "SELECT COALESCE(NULL, \"left\"), NULLIF(\"left\", \"right\"), GREATEST(\"left\", \"right\"), LEAST(\"left\", \"right\"), ROW(\"left\", \"right\"), ANY(ARRAY[$1, $2]), ALL(ARRAY[$1, $2]), CASE WHEN (\"left\" > $3) THEN $4 ELSE $5 END, \"percentile_cont\"($6) WITHIN GROUP (ORDER BY \"score\" DESC), EXTRACT(YEAR FROM \"created_at\"), (\"tags\")[$7], (\"name\" COLLATE \"C\") FROM \"metrics\"");
    EXPECT_EQ(statement.params().size(), std::size_t{7});

    db_query maria_array;
    const auto array_value = maria_array.array({maria_array.value(1)});
    maria_array.select(maria_array.any(array_value)).from("metrics");
    EXPECT_TRUE(testing::throws_on([&] { (void)maria_array.compile(db_driver::mariadb); }));

    db_query maria_within_group;
    const auto aggregate = maria_within_group.aggregate("percentile_cont", {maria_within_group.value(0.5)});
    const std::array maria_order{db_order_term{maria_within_group.column("score"), db_order_direction::asc}};
    maria_within_group.select(maria_within_group.within_group(aggregate, maria_order))
        .from("metrics");
    EXPECT_TRUE(testing::throws_on([&] { (void)maria_within_group.compile(db_driver::mariadb); }));

    EXPECT_TRUE(testing::throws_on([&] { (void)query.coalesce({}); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)query.greatest({}); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)query.least({}); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)query.within_group(left, {}); }));
    db_query empty_tuple;
    empty_tuple.select(empty_tuple.tuple({})).from("metrics");
    EXPECT_TRUE(testing::throws_on([&] { (void)empty_tuple.compile(db_driver::postgresql); }));
}

TEST(Ruvia, db_query_native_functions_and_identifier_quoting_are_dialect_safe) {
    db_query query;
    const auto amount = query.column("amount", "s");
    const auto name = query.column("name", "s");
    const auto count = query.aggregate("count", {query.star()});
    const auto sum = query.aggregate("sum", {amount});
    const db_order_term window_order[]{{query.column("id", "s"), db_order_direction::asc}};
    const auto row_number = query.over(query.call("row_number"), {.order_by_ = window_order});
    const auto scalar = query.call("lower", {name});
    query.select({count, sum, row_number, scalar}).from("sales", "s");

    const auto postgres = query.compile(db_driver::postgresql);
    EXPECT_EQ(postgres.sql(),
        "SELECT \"count\"(*), \"sum\"(\"s\".\"amount\"), \"row_number\"() OVER (ORDER BY \"s\".\"id\" ASC), \"lower\"(\"s\".\"name\") FROM \"sales\" AS \"s\"");

    const auto maria = query.compile(db_driver::mariadb);
    EXPECT_TRUE(maria.sql().find("count(*)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("sum(`s`.`amount`)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("row_number() OVER (ORDER BY `s`.`id` ASC)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("lower(`s`.`name`)") != std::string_view::npos);
    EXPECT_TRUE(maria.sql().find("`count`") == std::string_view::npos);

    db_query identifiers;
    const auto dangerous_column = identifiers.column("name\"; DROP TABLE users --", "tenant.schema");
    const auto dangerous_function = identifiers.call("pkg.weird\"function", {dangerous_column});
    const auto special_function = identifiers.call("weird`fn--", {dangerous_column});
    identifiers.select({dangerous_function, special_function}).from("tenant.schema", "s");
    const auto safe_postgres = identifiers.compile(db_driver::postgresql);
    EXPECT_TRUE(safe_postgres.sql().find("\"tenant\".\"schema\".\"name\"\"; DROP TABLE users --\"") != std::string_view::npos);
    EXPECT_TRUE(safe_postgres.sql().find("\"pkg\".\"weird\"\"function\"") != std::string_view::npos);
    const auto safe_maria = identifiers.compile(db_driver::mariadb);
    EXPECT_TRUE(safe_maria.sql().find("`tenant`.`schema`.`name\"; DROP TABLE users --`") != std::string_view::npos);
    EXPECT_TRUE(safe_maria.sql().find("`pkg`.`weird\"function`") != std::string_view::npos);
    EXPECT_TRUE(safe_maria.sql().find("`weird``fn--`") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)identifiers.column(std::string_view("bad\0name", 8)); }));

    const std::string overlong(65, 'x');
    db_query overlong_query;
    overlong_query.select(overlong_query.call(overlong)).from("events");
    EXPECT_TRUE(testing::throws_on([&] { (void)overlong_query.compile(db_driver::postgresql); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)overlong_query.compile(db_driver::mariadb); }));

    // MariaDB counts characters, PostgreSQL counts bytes: 64 two-byte "é"
    // fit MariaDB but not PostgreSQL's 63 bytes.
    std::string accented{};
    for (int i = 0; i < 64; ++i) {
        accented += "\xC3\xA9";
    }
    db_query accented_query;
    accented_query.select(accented_query.call(accented)).from("events");
    const auto accented_statement = accented_query.compile(db_driver::mariadb);
    EXPECT_TRUE(accented_statement.sql().find(accented) != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)accented_query.compile(db_driver::postgresql); }));
    db_query too_many_characters;
    too_many_characters.select(too_many_characters.call(accented + "x")).from("events");
    EXPECT_TRUE(testing::throws_on([&] { (void)too_many_characters.compile(db_driver::mariadb); }));
    db_query ill_formed;
    ill_formed.select(ill_formed.call("bad\xC3name")).from("events");
    EXPECT_TRUE(testing::throws_on([&] { (void)ill_formed.compile(db_driver::mariadb); }));
}

TEST(Ruvia, db_query_dml_sources_distinct_and_query_joins_compile) {
    db_query source;
    source.select({source.column("id", "s"), source.column("name", "s")}).from("source", "s");

    db_query insert;
    insert.insert_into("archive", {"id", "name"}).insert_from(source).returning({insert.column("id")});
    const auto inserted = insert.compile(db_driver::postgresql);
    EXPECT_EQ(inserted.sql(),
        "INSERT INTO \"archive\" (\"id\", \"name\") SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\" RETURNING \"id\"");

    db_query update;
    update.update("target", "t").set("name", update.value("changed"));
    update.update_from("source", "s").where(update.binary(update.column("id", "t"), op_type::equal, update.column("id", "s")));
    const auto updated = update.compile(db_driver::postgresql);
    EXPECT_EQ(updated.sql(),
        "UPDATE \"target\" AS \"t\" SET \"name\" = $1 FROM \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query removed;
    removed.delete_from("target", "t").delete_using("source", "s");
    removed.where(removed.binary(removed.column("id", "t"), op_type::equal, removed.column("id", "s")));
    const auto deleted = removed.compile(db_driver::postgresql);
    EXPECT_EQ(deleted.sql(),
        "DELETE FROM \"target\" AS \"t\" USING \"source\" AS \"s\" WHERE (\"t\".\"id\" = \"s\".\"id\")");

    db_query distinct;
    distinct.select({distinct.column("account_id"), distinct.column("id")})
        .distinct_on({distinct.column("account_id")})
        .from("events");
    const auto distinct_statement = distinct.compile(db_driver::postgresql);
    EXPECT_EQ(distinct_statement.sql(),
        "SELECT DISTINCT ON (\"account_id\") \"account_id\", \"id\" FROM \"events\"");
    EXPECT_TRUE(testing::throws_on([&] { (void)distinct.compile(db_driver::mariadb); }));

    db_query joined;
    joined.select(joined.star("base")).from("base", "base");
    const auto join_on = joined.binary(joined.column("id", "base"), op_type::equal, joined.column("id", "derived"));
    joined.join(db_join_type::left, source, join_on, "derived");
    const std::array<std::string_view, 1> using_columns{"id"};
    joined.join(db_join_type::inner, "same_ids", {}, "same_ids", using_columns);
    const auto joined_statement = joined.compile(db_driver::postgresql);
    EXPECT_EQ(joined_statement.sql(),
        "SELECT \"base\".* FROM \"base\" AS \"base\" LEFT JOIN (SELECT \"s\".\"id\", \"s\".\"name\" FROM \"source\" AS \"s\") AS \"derived\" ON (\"base\".\"id\" = \"derived\".\"id\") INNER JOIN \"same_ids\" AS \"same_ids\" USING (\"id\")");

    db_query function_source;
    const auto function = function_source.call("jsonb_each", {function_source.column("payload")});
    const db_source_column function_columns[]{{.name_ = "key"}, {.name_ = "value"}};
    function_source.select(function_source.star("kv"))
        .from_function(function, "kv",
            {.lateral_ = true, .with_ordinality_ = true, .columns_ = function_columns});
    const auto function_statement = function_source.compile(db_driver::postgresql);
    EXPECT_TRUE(function_statement.sql().find(
                    "FROM LATERAL \"jsonb_each\"(\"payload\") WITH ORDINALITY AS \"kv\" (\"key\", \"value\")") != std::string_view::npos);
}

TEST(Ruvia, db_query_bulk_upsert_conditions_and_returning) {
    db_query query;
    const std::array<std::string_view, 2> columns{"id", "revision"};
    query.insert_into("device", columns, "d");
    const std::array first{query.value(db_value(7)), query.value(db_value(2))};
    const std::array second{query.value(db_value(8)), query.default_value()};
    query.values(first).values(second);
    const auto revision = query.column("revision", "excluded");
    const borrowed_text conflict_columns[]{"id"};
    const db_assignment updates[]{{"revision", revision}};
    query.on_conflict({.columns_ = conflict_columns, .update_ = updates, .update_where_ = query.binary(revision, op_type::greater, query.column("revision", "d"))});
    const std::array returned{query.column("id")};
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
    const std::array row{upsert.value(db_value(7)), upsert.value(db_value(2))};
    upsert.values(row);
    const db_assignment updates[]{{"revision", upsert.excluded("revision")}};
    upsert.on_conflict({.update_ = updates, .any_unique_key_ = true});
    const std::array returned{upsert.column("id"), upsert.alias(upsert.column("revision"), "rev")};
    upsert.returning(returned);
    const auto inserted = upsert.compile(db_driver::mariadb);
    EXPECT_EQ(inserted.sql(), "INSERT INTO `device` (`id`, `revision`) VALUES (?, ?) ON DUPLICATE KEY UPDATE `revision` = VALUES(`revision`) RETURNING `id`, `revision` AS `rev`");
    EXPECT_TRUE(inserted.returns_rows());

    db_query removed;
    removed.delete_from("device").where(removed.binary(removed.column("id"), op_type::equal, removed.value(db_value(7))));
    const std::array removed_columns{removed.column("id")};
    removed.returning(removed_columns);
    const auto deleted = removed.compile(db_driver::mariadb);
    EXPECT_EQ(deleted.sql(), "DELETE FROM `device` WHERE (`id` = ?) RETURNING `id`");

    db_query updated;
    updated.update("device").set("revision", updated.value(db_value(3)));
    const std::array updated_columns{updated.column("id")};
    updated.returning(updated_columns);
    const auto updated_postgres = updated.compile(db_driver::postgresql);
    EXPECT_TRUE(updated_postgres.returns_rows());
    EXPECT_TRUE(testing::throws_on([&] { (void)updated.compile(db_driver::mariadb); }));
}

TEST(Ruvia, db_query_set_operations_preserve_grouping_and_nested_limits) {
    db_query first, second, third;
    first.select(first.value(db_value(1)));
    second.select(second.value(db_value(2))).limit(1);
    third.select(third.value(db_value(3)));
    first.combine(db_set_operation::union_all, second).combine(db_set_operation::intersect, third).limit(4);
    const auto statement = first.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "((SELECT $1) UNION ALL (SELECT $2 LIMIT $3)) INTERSECT (SELECT $4) LIMIT $5");
    EXPECT_EQ(statement.params().size(), std::size_t{5});
}

TEST(Ruvia, db_query_mariadb_uses_its_own_placeholders_and_null_ordering) {
    db_query query;
    const auto name = query.column("name");
    query.select(query.binary(name, op_type::concat, query.value(db_value("suffix")))).from("user");
    query.order_by(name, db_order_direction::asc, db_nulls_order::last).offset(2);
    const auto statement = query.compile(db_driver::mariadb);
    EXPECT_EQ(statement.sql(), "SELECT CONCAT(`name`, ?) FROM `user` ORDER BY (`name` IS NULL) ASC, `name` ASC LIMIT 18446744073709551615 OFFSET ?");
    query.where(query.binary(name, op_type::i_like, query.value(db_value("needle"))));
    EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
}

TEST(Ruvia, db_query_rejects_foreign_expressions_and_incompatible_clauses) {
    db_query first, second;
    auto foreign = first.column("id");
    EXPECT_TRUE(testing::throws_on([&] { second.where(foreign); }));
    second.where(second.import_expression(foreign)).insert_into("device");
    EXPECT_TRUE(testing::throws_on([&] { (void)second.compile(db_driver::postgresql); }));
    db_query invalid_join;
    invalid_join.from("one").join(db_join_type::left, "two");
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid_join.compile(db_driver::postgresql); }));
}

TEST(Ruvia, db_query_data_modifications_require_top_level_or_cte_placement) {
    db_query removed;
    removed.delete_from("device").returning({removed.column("id")});
    db_query query;
    EXPECT_TRUE(testing::throws_on([&] { (void)query.subquery(removed); }));
    EXPECT_TRUE(testing::throws_on([&] { query.from(removed, "removed"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.join(db_join_type::cross, removed, {}, "removed"); }));
    db_query insert;
    insert.insert_into("archive", {"id"});
    EXPECT_TRUE(testing::throws_on([&] { insert.insert_from(removed); }));
    query.with("removed", removed).from("removed");
    EXPECT_EQ(query.compile(db_driver::postgresql).returns_rows(), true);
    db_query nested;
    EXPECT_TRUE(testing::throws_on([&] { nested.from(query, "nested"); }));
    nested.with("nested", query).from("nested");
    EXPECT_TRUE(testing::throws_on([&] { (void)nested.compile(db_driver::postgresql); }));
}

TEST(Ruvia, db_query_set_operands_cannot_hide_write_ctes_from_subquery_admission) {
    db_query removed;
    removed.delete_from("device").returning({removed.column("id")});
    db_query branch;
    branch.select(branch.value(1)).with("removed", removed);
    db_query combined;
    combined.select(combined.value(0)).combine(db_set_operation::union_all, branch);
    EXPECT_TRUE(combined.has_writes());
    EXPECT_TRUE(combined.clone().has_writes());
    db_query moved(std::move(combined));
    EXPECT_TRUE(moved.has_writes());
    for (unsigned operation = 0; operation != 4; ++operation) {
        db_query outer;
        outer.select(outer.value(7)).from("retained");
        bool rejected = false;
        try {
            switch (operation) {
                case 0:
                    outer.from(moved, "hidden");
                    break;
                case 1:
                    (void)outer.subquery(moved);
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

RUVIA_DB_ENTITY(tagged_type, "tagged", db_column<"id", std::int64_t>,
    db_column<"tags", std::vector<std::string>>)
RUVIA_DB_ENTITY(uuids_type, "uuid_tags",
    db_column<"tags", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::uuid}>)
RUVIA_DB_ENTITY(codes_type, "codes",
    db_column<"values", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::varchar, .length_ = 32}>)

TEST(Ruvia, db_predicate_between_and_array_operators_bind_owned_typed_values) {
    std::array<std::string, 2> input{std::string(200, 'a'), std::string("zone-b")};
    auto condition = tagged_type::column<"id">().between(3, 8) && tagged_type::column<"tags">().array_contains(std::span<const std::string>(input));
    input[0].assign(200, 'z');
    db_query query;
    query.from("tagged").where(condition.expression(query));
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(), "SELECT * FROM \"tagged\" WHERE ((\"tagged\".\"id\" BETWEEN $1 AND $2) AND (\"tagged\".\"tags\" @> CAST(ARRAY[$3, $4] AS TEXT[])))");
    EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));

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
        query.select(query.binary(query.column("lhs"), item.op_, query.column("rhs"))).from("cq_operator_matrix");
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.token_) != std::string_view::npos);
        EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
    }

    db_query nulls;
    const auto id = nulls.column("id");
    nulls.select({nulls.binary(id, op_type::equal, nulls.null_value()),
                     nulls.binary(nulls.null_value(), op_type::not_equal, id),
                     nulls.binary(id, op_type::in, nulls.list({})),
                     nulls.binary(id, op_type::not_in, nulls.list({}))})
        .from("cq_operator_matrix");
    const auto null_statement = nulls.compile(db_driver::postgresql);
    EXPECT_TRUE(null_statement.sql().find(" IS NULL") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find(" IS NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find("FALSE") != std::string_view::npos);
    EXPECT_TRUE(null_statement.sql().find("TRUE") != std::string_view::npos);

    db_query invalid_in;
    invalid_in.select(invalid_in.binary(invalid_in.column("id"), op_type::in, invalid_in.value(1))).from("cq_operator_matrix");
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid_in.compile(db_driver::postgresql); }));
}

TEST(Ruvia, db_query_unary_operators_and_parameter_modes_cover_all_semantics) {
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
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.token_) != std::string_view::npos);
        const auto maria = query.compile(db_driver::mariadb);
        EXPECT_TRUE(maria.sql().find(item.token_) != std::string_view::npos);
    }

    db_query literal;
    const auto literal_value = literal.value(db_value("O'Reilly"));
    literal.select(literal_value).from("cq_unary_matrix");
    const auto bound = literal.compile(db_driver::postgresql, db_parameter_mode::bound);
    EXPECT_EQ(bound.params().size(), std::size_t{1});
    const auto rendered = literal.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_EQ(rendered.params().size(), std::size_t{0});
    EXPECT_TRUE(rendered.sql().find("E'O''Reilly'") != std::string_view::npos);
    const auto maria_rendered = literal.compile(db_driver::mariadb, db_parameter_mode::literal);
    EXPECT_TRUE(maria_rendered.sql().find("CONVERT(X'4f275265696c6c79' USING utf8mb4)") != std::string_view::npos);
    EXPECT_EQ(db_query::render_expression(literal_value, db_driver::postgresql), "E'O''Reilly'");

    db_query invalid;
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid.unary(db_unary_operator::not_value, {}); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid.binary({}, op_type::equal, invalid.value(1)); }));
}

TEST(Ruvia, db_query_unary_minus_and_bit_not_keep_negative_literals_out_of_comments) {
    db_query query;
    const auto negated = query.unary(db_unary_operator::negate, query.value(-5));
    const auto inverted = query.unary(db_unary_operator::bit_not, query.value(-5));
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        const auto negated_sql = db_query::render_expression(negated, driver);
        EXPECT_EQ(negated_sql, "(- -5)");
        EXPECT_TRUE(negated_sql.find("--") == std::string_view::npos);
        EXPECT_EQ(db_query::render_expression(inverted, driver),
            driver == db_driver::postgresql ? "(~ (-5)::int8)" : "(~ -5)");
    }
}

RUVIA_DB_ENTITY(cq_device_type, "cq_device", db_column<"id", std::int64_t>, db_column<"name", std::string>,
    db_column<"tags", std::vector<std::string>>)

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
    EXPECT_TRUE(testing::throws_on([&] { (void)compile(tagged_type::column<"name">().ilike("a%"), db_driver::mariadb); }));
    EXPECT_TRUE(compile_sql([&] { return compile(tagged_type::column<"id">().is_null(), db_driver::postgresql); }).find(" IS NULL") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] { return compile(tagged_type::column<"id">().is_not_null(), db_driver::postgresql); }).find(" IS NOT NULL") != std::string_view::npos);

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
    EXPECT_TRUE(testing::throws_on([&] { (void)compile(tagged_type::column<"tags">().array_contains({"one"}), db_driver::mariadb); }));

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
    EXPECT_TRUE(testing::throws_on([&] { (void)(std::move(empty_and) && (tagged_type::column<"id">() == 1)); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)(std::move(empty_or) || (tagged_type::column<"id">() == 1)); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)!std::move(empty); }));
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
        EXPECT_TRUE(testing::throws_on([&] { (void)!std::move(source); }));
        source = entity_type::column<"id">() == 1;
        query.from("device").where(source.expression(query));
        const auto statement = query.compile(db_driver::postgresql);
        EXPECT_EQ(statement.sql(),
            "SELECT * FROM \"device\" WHERE (\"device\".\"id\" = $1)");
    }
}

TEST(Ruvia, db_query_clause_source_and_join_methods_cover_state_and_dialect_rules) {
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
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(),
             std::numeric_limits<std::int64_t>::min() + 5, std::int64_t{-1}}) {
        EXPECT_TRUE(testing::throws_on([&] { query.limit(invalid); }));
        EXPECT_TRUE(testing::throws_on([&] { query.offset(invalid); }));
    }
    for (const auto above_bigint : {static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1,
             (std::numeric_limits<std::uint64_t>::max)()}) {
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
    derived.select(derived.column("id")).from("cq_source");
    db_query source_options;
    const db_source_column derived_columns[]{{.name_ = "id"}};
    source_options.select(source_options.star("d")).from(derived, "d", {.columns_ = derived_columns});
    const auto source_statement = source_options.compile(db_driver::postgresql);
    EXPECT_TRUE(source_statement.sql().find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"d\" (\"id\")") != std::string_view::npos);
    const auto maria_source_statement = source_options.compile(db_driver::mariadb);
    EXPECT_TRUE(maria_source_statement.sql().find("FROM (SELECT `id` FROM `cq_source`) AS `d` (`id`)") != std::string_view::npos);

    db_query record_source;
    const auto record_function = record_source.call("jsonb_to_record", {record_source.value(R"({"id":1})")});
    const db_source_column record_columns[]{{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}},
        {.name_ = "name", .type_ = {.data_type_ = db_data_type::varchar, .length_ = 8}}};
    record_source.select(record_source.star("r"))
        .from_function(record_function, "r",
            {.columns_ = record_columns});
    const auto record_statement = record_source.compile(db_driver::postgresql);
    EXPECT_TRUE(record_statement.sql().find("AS \"r\" (\"id\" INTEGER, \"name\" VARCHAR(8))") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)record_source.compile(db_driver::mariadb); }));

    db_query lateral;
    lateral.select(lateral.star("d")).from(derived, "d", {.lateral_ = true});
    EXPECT_TRUE(compile_sql([&] { return lateral.compile(db_driver::postgresql); }).find("FROM LATERAL") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)lateral.compile(db_driver::mariadb); }));
    db_query bad_ordinality;
    bad_ordinality.select(bad_ordinality.star("d")).from(derived, "d", {.with_ordinality_ = true});
    EXPECT_TRUE(testing::throws_on([&] { (void)bad_ordinality.compile(db_driver::postgresql); }));

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
        const auto postgres = joined.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(token) != std::string_view::npos);
        if (type == db_join_type::full) {
            EXPECT_TRUE(testing::throws_on([&] { (void)joined.compile(db_driver::mariadb); }));
        } else {
            const auto maria = joined.compile(db_driver::mariadb);
            EXPECT_TRUE(maria.sql().find(token) != std::string_view::npos);
        }
    }
    db_query cross;
    cross.select(cross.star("a")).from("cq_a", "a").join(db_join_type::cross, "cq_b", {}, "b");
    EXPECT_TRUE(compile_sql([&] { return cross.compile(db_driver::postgresql); }).find(" CROSS JOIN ") != std::string_view::npos);
    cross.join(db_join_type::cross, "cq_c", cross.column("id"), "c");
    EXPECT_TRUE(testing::throws_on([&] { (void)cross.compile(db_driver::postgresql); }));
}

TEST(Ruvia, db_query_dml_and_conflict_overloads_cover_postgresql_and_mariadb) {
    db_query insert;
    const std::array<std::string_view, 2> columns{"id", "name"};
    insert.insert_into("cq_devices", std::span<const std::string_view>(columns), "d");
    const std::array row{insert.value(1), insert.value("first")};
    insert.values(std::span<const db_expression>(row));
    const auto inserted = insert.compile(db_driver::postgresql);
    EXPECT_TRUE(inserted.sql().find("INSERT INTO \"cq_devices\" AS \"d\"") != std::string_view::npos);
    EXPECT_TRUE(!inserted.returns_rows());

    db_query defaults;
    defaults.insert_into("cq_devices");
    const auto default_pg = defaults.compile(db_driver::postgresql);
    EXPECT_TRUE(default_pg.sql().find("DEFAULT VALUES") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] { return defaults.compile(db_driver::mariadb); }).find("() VALUES ()") != std::string_view::npos);

    db_query source;
    source.select({source.column("id"), source.column("name")}).from("cq_source");
    db_query insert_from;
    insert_from.insert_into("cq_devices", {"id", "name"}).insert_from(source);
    EXPECT_TRUE(compile_sql([&] { return insert_from.compile(db_driver::postgresql); }).find("SELECT \"id\", \"name\"") != std::string_view::npos);

    db_query do_nothing;
    do_nothing.insert_into("cq_devices", {"id", "name"}).values({do_nothing.value(1), do_nothing.value("x")});
    const borrowed_text conflict_columns[]{"id"};
    do_nothing.on_conflict({.columns_ = conflict_columns, .do_nothing_ = true});
    EXPECT_TRUE(compile_sql([&] { return do_nothing.compile(db_driver::postgresql); }).find("ON CONFLICT (\"id\") DO NOTHING") != std::string_view::npos);

    db_query constraint;
    constraint.insert_into("cq_devices", {"id", "name"}).values({constraint.value(1), constraint.value("x")});
    constraint.on_conflict({.constraint_ = "cq_devices_pkey", .do_nothing_ = true});
    EXPECT_TRUE(compile_sql([&] { return constraint.compile(db_driver::postgresql); }).find("ON CONSTRAINT \"cq_devices_pkey\" DO NOTHING") != std::string_view::npos);

    db_query targeted;
    targeted.insert_into("cq_devices", {"id", "name"}).values({targeted.value(1), targeted.value("x")});
    const auto active = targeted.binary(targeted.column("active"), op_type::equal, targeted.value(true));
    const db_assignment targeted_updates[]{{"name", targeted.excluded("name")}};
    targeted.on_conflict({.columns_ = conflict_columns, .target_where_ = active, .update_ = targeted_updates, .update_where_ = targeted.binary(targeted.column("name"), op_type::not_equal, targeted.excluded("name"))});
    const auto targeted_statement = targeted.compile(db_driver::postgresql);
    EXPECT_TRUE(targeted_statement.sql().find("ON CONFLICT (\"id\") WHERE (\"active\" = $3) DO UPDATE SET") != std::string_view::npos);
    EXPECT_TRUE(targeted_statement.sql().find("WHERE (\"name\" <> ") != std::string_view::npos);
    EXPECT_TRUE(targeted_statement.sql().find("excluded.\"name\")") != std::string_view::npos ||
                targeted_statement.sql().find("\"excluded\".\"name\")") != std::string_view::npos);

    db_query maria_upsert;
    maria_upsert.insert_into("cq_devices", {"id", "name"}).values({maria_upsert.value(1), maria_upsert.value("x")});
    const db_assignment maria_updates[]{{"name", maria_upsert.excluded("name")}};
    maria_upsert.on_conflict({.update_ = maria_updates, .any_unique_key_ = true});
    const auto maria_statement = maria_upsert.compile(db_driver::mariadb);
    EXPECT_TRUE(maria_statement.sql().find("ON DUPLICATE KEY UPDATE \"name\"") == std::string_view::npos);
    EXPECT_TRUE(maria_statement.sql().find("ON DUPLICATE KEY UPDATE `name` = VALUES(`name`)") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)maria_upsert.compile(db_driver::postgresql); }));
    db_query invalid_conflict;
    invalid_conflict.insert_into("cq_devices", {"id"}).values({invalid_conflict.value(1)});
    invalid_conflict.on_conflict({.columns_ = conflict_columns, .constraint_ = "other", .do_nothing_ = true});
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid_conflict.compile(db_driver::postgresql); }));

    db_query update;
    update.update("cq_devices", "d").set("name", update.value("updated"));
    update.set("name", update.value("updated-again"));
    db_query update_from;
    update_from.select(update_from.column("id")).from("cq_source");
    update.update_from(update_from, "s").where(update.binary(update.column("id", "d"), op_type::equal, update.column("id", "s")));
    EXPECT_TRUE(compile_sql([&] { return update.compile(db_driver::postgresql); }).find("FROM (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);

    db_query remove;
    remove.delete_from("cq_devices", "d");
    db_query delete_source;
    delete_source.select(delete_source.column("id")).from("cq_source");
    remove.delete_using(delete_source, "s").where(remove.binary(remove.column("id", "d"), op_type::equal, remove.column("id", "s")));
    EXPECT_TRUE(compile_sql([&] { return remove.compile(db_driver::postgresql); }).find("USING (SELECT \"id\" FROM \"cq_source\") AS \"s\"") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)remove.compile(db_driver::mariadb); }));

    db_query returning;
    returning.insert_into("cq_devices", {"id"}).values({returning.value(1)}).returning({returning.column("id")});
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
        query.select(query.star("d")).from("cq_devices", "d").lock({.mode_ = item.mode_});
        const auto postgres = query.compile(db_driver::postgresql);
        EXPECT_TRUE(postgres.sql().find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_token_.empty()) {
            EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
        } else {
            EXPECT_TRUE(compile_sql([&] { return query.compile(db_driver::mariadb); }).find(item.maria_token_) != std::string_view::npos);
        }
    }
    db_query scoped_lock;
    const borrowed_text lock_tables[]{"d"};
    scoped_lock.select(scoped_lock.star("d")).from("cq_devices", "d").lock({.mode_ = db_row_lock::update, .nowait_ = true, .tables_ = lock_tables});
    EXPECT_TRUE(compile_sql([&] { return scoped_lock.compile(db_driver::postgresql); }).find("FOR UPDATE OF \"d\" NOWAIT") != std::string_view::npos);
    db_query skip_locked;
    skip_locked.select(skip_locked.star()).from("cq_devices").lock({.skip_locked_ = true});
    EXPECT_TRUE(compile_sql([&] { return skip_locked.compile(db_driver::postgresql); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    EXPECT_TRUE(compile_sql([&] { return skip_locked.compile(db_driver::mariadb); }).find("FOR UPDATE SKIP LOCKED") != std::string_view::npos);
    db_query cleared;
    cleared.select(cleared.star()).from("cq_devices").lock({}).clear_lock();
    EXPECT_TRUE(compile_sql([&] { return cleared.compile(db_driver::postgresql); }).find(" FOR ") == std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { cleared.lock({.nowait_ = true, .skip_locked_ = true}); }));
    db_query shared_skip;
    shared_skip.select(shared_skip.star()).from("cq_devices").lock({.mode_ = db_row_lock::share, .skip_locked_ = true});
    EXPECT_TRUE(testing::throws_on([&] { (void)shared_skip.compile(db_driver::mariadb); }));

    db_query cte_body;
    cte_body.select(cte_body.value(1)).from("cq_seed");
    db_query recursive;
    const borrowed_text cte_columns[]{"n"};
    recursive.with("numbers", cte_body, {.recursive_ = true, .materialization_ = db_materialization::not_materialized, .columns_ = cte_columns})
        .select(recursive.column("n"))
        .from("numbers");
    const auto recursive_statement = recursive.compile(db_driver::postgresql);
    EXPECT_TRUE(recursive_statement.sql().find("WITH RECURSIVE \"numbers\" (\"n\") AS NOT MATERIALIZED") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)recursive.compile(db_driver::mariadb); }));
    db_query duplicate_cte;
    duplicate_cte.with("one", cte_body);
    EXPECT_TRUE(testing::throws_on([&] { duplicate_cte.with("one", cte_body); }));

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
        EXPECT_TRUE(compile_sql([&] { return first.compile(db_driver::postgresql); }).find(item.postgres_token_) != std::string_view::npos);
        if (item.maria_supported_) {
            EXPECT_TRUE(compile_sql([&] { return first.compile(db_driver::mariadb); }).find(item.postgres_token_) != std::string_view::npos);
        } else {
            EXPECT_TRUE(testing::throws_on([&] { (void)first.compile(db_driver::mariadb); }));
        }
    }
}

TEST(Ruvia, db_query_window_frame_and_date_part_enumerations_cover_each_value) {
    const std::array frames{db_window_frame::rows, db_window_frame::range, db_window_frame::groups};
    for (const auto frame : frames) {
        db_query query;
        query.select(query.over(query.call("row_number"),
                         {.frame_ = db_window_frame_options{.kind_ = frame,
                              .start_ = {db_frame_boundary::unbounded_preceding, 0},
                              .end_ = {db_frame_boundary::current_row, 0}}}))
            .from("cq_window");
        const auto postgres = query.compile(db_driver::postgresql);
        const auto token = frame == db_window_frame::rows ? "ROWS" : frame == db_window_frame::range ? "RANGE"
                                                                                                     : "GROUPS";
        EXPECT_TRUE(postgres.sql().find(token) != std::string_view::npos);
        if (frame == db_window_frame::groups) {
            EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
        } else {
            EXPECT_TRUE(compile_sql([&] { return query.compile(db_driver::mariadb); }).find(token) != std::string_view::npos);
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
        EXPECT_TRUE(compile_sql([&] { return query.compile(db_driver::postgresql); }).find(token) != std::string_view::npos);
    }
    db_query invalid_boundary;
    invalid_boundary.select(invalid_boundary.over(invalid_boundary.call("row_number"),
                                {.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::current_row, 1}}}))
        .from("cq_window");
    EXPECT_TRUE(testing::throws_on([&] { (void)invalid_boundary.compile(db_driver::postgresql); }));
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(testing::throws_on([&] { (void)db_window_boundary{db_frame_boundary::preceding, invalid}; }));
    }
    EXPECT_TRUE(testing::throws_on([&] {
        (void)db_window_boundary{
            db_frame_boundary::following, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1};
    }));
    db_query reversed;
    reversed.select(reversed.over(reversed.call("row_number"),
                        {.frame_ = db_window_frame_options{.start_ = {db_frame_boundary::following, 0},
                             .end_ = {db_frame_boundary::preceding, 0}}}))
        .from("cq_window");
    EXPECT_TRUE(testing::throws_on([&] { (void)reversed.compile(db_driver::postgresql); }));

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
        EXPECT_TRUE(compile_sql([&] { return query.compile(db_driver::postgresql); }).find(item.token_) != std::string_view::npos);
        if (item.maria_supported_) {
            EXPECT_TRUE(compile_sql([&] { return query.compile(db_driver::mariadb); }).find(item.token_) != std::string_view::npos);
        } else {
            EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
        }
    }
}

TEST(Ruvia, db_query_expression_ownership_import_subquery_cast_and_cache_methods_are_observable) {
    db_query source;
    const auto qualified = source.column("id", "source");
    db_query target;
    const auto remapped = target.import_expression(qualified, "source", "target");
    target.select(remapped).from("cq_devices", "target");
    EXPECT_EQ(compile_sql([&] { return target.compile(db_driver::postgresql); }), "SELECT \"target\".\"id\" FROM \"cq_devices\" AS \"target\"");
    EXPECT_TRUE(testing::throws_on([&] { (void)target.import_expression(qualified, "source", {}); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)target.import_expression({}, "source", "target"); }));

    db_query child;
    child.select(child.value(1)).from("cq_devices");
    db_query nested;
    const auto exists = nested.exists(child);
    const auto scalar = nested.subquery(child);
    nested.select({exists, scalar}).from("cq_devices");
    const auto nested_statement = nested.compile(db_driver::postgresql);
    EXPECT_TRUE(nested_statement.sql().find("EXISTS (SELECT $1 FROM \"cq_devices\")") != std::string_view::npos);
    EXPECT_TRUE(nested_statement.sql().find("(SELECT $2 FROM \"cq_devices\")") != std::string_view::npos);
    db_query dml;
    dml.delete_from("cq_devices").returning({dml.column("id")});
    EXPECT_TRUE(testing::throws_on([&] { (void)nested.subquery(dml); }));

    db_query typed;
    const auto number = typed.cast(typed.value(1), db_data_type::integer);
    const auto sized = typed.cast(typed.value("x"), db_type_definition{.data_type_ = db_data_type::varchar, .length_ = 8});
    typed.select({number, sized}).from("cq_devices");
    const auto typed_pg = typed.compile(db_driver::postgresql);
    EXPECT_TRUE(typed_pg.sql().find("CAST($1 AS INTEGER)") != std::string_view::npos);
    EXPECT_TRUE(typed_pg.sql().find("CAST($2 AS VARCHAR(8))") != std::string_view::npos);
    db_query named_type;
    named_type.select(named_type.cast(named_type.value(1), db_type_definition{.custom_name_ = "cq_custom_type"})).from("cq_devices");
    EXPECT_TRUE(compile_sql([&] { return named_type.compile(db_driver::postgresql); }).find("CAST($1 AS \"cq_custom_type\")") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)named_type.compile(db_driver::mariadb); }));

    db_query named;
    const std::array<db_named_argument, 1> named_arguments{{db_named_argument{"precision", named.value(2)}}};
    named.select(named.call("round", {}, named_arguments)).from("cq_devices");
    EXPECT_TRUE(compile_sql([&] { return named.compile(db_driver::postgresql); }).find("\"precision\" => $1") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)named.compile(db_driver::mariadb); }));
    EXPECT_TRUE(testing::throws_on([&] {
        const std::array<db_named_argument, 2> duplicate{{db_named_argument{"x", named.value(1)}, db_named_argument{"x", named.value(2)}}};
        (void)named.call("round", {}, duplicate);
    }));

    db_query state;
    EXPECT_TRUE(state.returns_rows());
    EXPECT_TRUE(!state.has_where());
    EXPECT_TRUE(!state.has_grouping());
    state.cache(false).cache(std::chrono::milliseconds(50)).cache(db_cache_options{.id_ = "cq-cache", .duration_ = std::chrono::milliseconds(25)});
    state.select(state.value(1)).from("cq_devices");
    const auto state_statement = state.compile(db_driver::postgresql);
    EXPECT_TRUE(state_statement.params().size() == std::size_t{1});
    EXPECT_TRUE(testing::throws_on([&] { state.cache("bad", std::chrono::milliseconds(0)); }));
    EXPECT_TRUE(testing::throws_on([&] { state.cache("bad", std::chrono::milliseconds(-1)); }));

    auto clone = state.clone();
    const auto clone_sql = compile_sql([&] { return clone.compile(db_driver::postgresql); });
    const auto state_sql = compile_sql([&] { return state.compile(db_driver::postgresql); });
    EXPECT_EQ(clone_sql, state_sql);
    db_query moved = std::move(clone);
    EXPECT_TRUE(testing::throws_on([&] { (void)clone.returns_rows(); }));
    db_query assigned;
    assigned = std::move(moved);
    EXPECT_TRUE(assigned.returns_rows());
    EXPECT_TRUE(testing::throws_on([&] { (void)moved.returns_rows(); }));

    db_query values;
    values.values({values.value(1), values.value(2)});
    EXPECT_TRUE(values.returns_rows());
    db_query dml_insert;
    dml_insert.insert_into("cq_devices", {"id"}).values({dml_insert.value(1)});
    EXPECT_TRUE(!dml_insert.returns_rows());
    dml_insert.returning({dml_insert.column("id")});
    EXPECT_TRUE(dml_insert.returns_rows());
}

RUVIA_DB_ENTITY(char_codes_type, "char_codes",
    db_column<"values", std::vector<std::string>, db_column_options{.data_type_ = db_data_type::char_value, .length_ = 8}>)

TEST(Ruvia, db_query_char_cast_and_array_predicate_render_for_supported_dialects) {
    db_query casts;
    casts.select(casts.cast(casts.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 64})).from("char_records");
    const auto postgres = casts.compile(db_driver::postgresql);
    EXPECT_TRUE(postgres.sql().find("CAST($1 AS CHAR(64))") != std::string_view::npos);

    const auto maria = casts.compile(db_driver::mariadb);
    EXPECT_TRUE(maria.sql().find("CAST(? AS CHAR(64))") != std::string_view::npos);

    db_query long_maria_cast;
    long_maria_cast.select(long_maria_cast.cast(long_maria_cast.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 256})).from("char_records");
    const auto long_maria_statement = long_maria_cast.compile(db_driver::mariadb);
    EXPECT_TRUE(long_maria_statement.sql().find("AS CHAR(256)") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] {
        db_query invalid;
        invalid.select(invalid.cast(invalid.value("x"), db_type_definition{.data_type_ = db_data_type::char_value, .length_ = 10485761})).from("char_records");
        (void)invalid.compile(db_driver::postgresql);
    }));

    db_query array;
    auto contains = char_codes_type::column<"values">().array_contains({"zone-a"});
    array.from("char_codes").where(contains.expression(array));
    const auto array_statement = array.compile(db_driver::postgresql);
    EXPECT_TRUE(array_statement.sql().find("AS CHAR(8)[]") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)array.compile(db_driver::mariadb); }));
}

TEST(Ruvia, db_query_span_overloads_cover_expression_and_statement_builders) {
    db_query query;
    const auto left = query.column("left");
    const auto right = query.column("right");
    const std::array<db_expression, 2> expressions{left, right};
    const auto call = query.call("coalesce", std::span<const db_expression>(expressions));
    const auto aggregate = query.aggregate("count", std::span<const db_expression>(expressions));
    const auto coalesced = query.coalesce(std::span<const db_expression>(expressions));
    const auto greatest = query.greatest(std::span<const db_expression>(expressions));
    const auto least = query.least(std::span<const db_expression>(expressions));
    const auto tuple = query.tuple(std::span<const db_expression>(expressions));
    const auto list = query.list(std::span<const db_expression>(expressions));
    const auto array_value = query.array(std::span<const db_expression>(expressions));
    const std::array<db_case_branch, 1> branches{{{query.binary(left, db_binary_operator::equal, query.value(1)), right}}};
    const auto case_expression = query.case_when(std::span<const db_case_branch>(branches), query.value(0));
    const std::array<db_order_term, 1> order{{{left, db_order_direction::asc, db_nulls_order::default_value}}};
    const auto within = query.within_group(aggregate, std::span<const db_order_term>(order));
    query.select(std::span<const db_expression>(expressions)).add_select(call).from("cq_span").group_by(std::span<const db_expression>(&left, 1)).add_group_by(right).having(query.unary(db_unary_operator::is_not_null, coalesced)).and_having(query.binary(greatest, db_binary_operator::greater, least)).order_by(left).add_order_by(right);
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_TRUE(statement.sql().find("GROUP BY") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().find("HAVING") != std::string_view::npos);

    db_query distinct;
    const auto distinct_left = distinct.column("left");
    distinct.select(distinct_left);
    const std::array<db_expression, 1> distinct_expressions{distinct.column("left")};
    distinct.distinct_on(std::span<const db_expression>(distinct_expressions)).from("cq_span");
    EXPECT_TRUE(compile_sql([&] { return distinct.compile(db_driver::postgresql); }).find("DISTINCT ON") != std::string_view::npos);

    db_query dml;
    const std::array<std::string_view, 1> columns{"value"};
    const std::array<db_expression, 1> row{dml.value(1)};
    dml.insert_into("cq_span", std::span<const std::string_view>(columns)).values(std::span<const db_expression>(row));
    const std::array<db_expression, 1> returned{dml.column("value")};
    dml.returning(std::span<const db_expression>(returned));
    EXPECT_TRUE(dml.compile(db_driver::postgresql).returns_rows());

    (void)tuple;
    (void)list;
    (void)array_value;
    (void)case_expression;
    (void)within;
}

TEST(Ruvia, db_query_import_expression_rewrites_correlated_subquery_qualifiers) {
    db_query sub;
    sub.select(sub.value(1))
        .from("orders", "o")
        .where(sub.binary(sub.column("user_id", "o"), db_binary_operator::equal, sub.column("id", "users")));
    db_query builder;
    const auto exists_expr = builder.exists(sub);
    builder.select(exists_expr);
    const auto original = compile_sql([&] { return builder.compile(db_driver::postgresql); });

    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(exists_expr, "users", "u"));
    const auto compiled = main_query.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"o\" WHERE (\"o\".\"user_id\" = \"u\".\"id\"))");
    EXPECT_EQ(compile_sql([&] { return builder.compile(db_driver::postgresql); }), original);
}

TEST(Ruvia, db_query_import_expression_preserves_schema_qualified_local_bindings) {
    for (const std::string_view qualifier : {"users", "public.users"}) {
        db_query main_query;
        main_query.select(main_query.column("id", "u")).from("users", "u");
        {
            db_query sub;
            sub.select(sub.value(1))
                .from("public.users")
                .where(sub.binary(sub.column("id", qualifier), op_type::equal, sub.value(10)));
            db_query builder;
            const auto expression = builder.exists(sub);
            main_query.where(main_query.import_expression(expression, qualifier, "u"));
        }
        const auto compiled = main_query.compile(db_driver::postgresql);
        const std::string expected(qualifier == "users"
                                       ? "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"public\".\"users\" WHERE (\"users\".\"id\" = $2))"
                                       : "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"public\".\"users\" WHERE (\"public\".\"users\".\"id\" = $2))");
        EXPECT_EQ(compiled.sql(), expected);
        EXPECT_EQ(compiled.params().size(), std::size_t{2});
    }
}

TEST(Ruvia, db_query_import_expression_rejects_captured_correlated_references_only) {
    db_query sub;
    sub.select(sub.value(1))
        .from("orders", "u")
        .where(sub.binary(sub.column("user_id", "u"), op_type::equal, sub.column("id", "users")));
    db_query builder;
    const auto expression = builder.exists(sub);
    builder.select(expression);
    const auto original = compile_sql([&] { return builder.compile(db_driver::postgresql); });
    db_query main_query;
    main_query.select(main_query.column("id", "u")).from("users", "u");
    bool rejected = false;
    try {
        (void)main_query.import_expression(expression, "users", "u");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(compile_sql([&] { return builder.compile(db_driver::postgresql); }), original);

    // The abandoned correlated predicate still exists in the builder's storage,
    // but does not appear in the imported query and must not cause a rejection.
    sub.where(sub.binary(sub.column("user_id", "u"), op_type::equal, sub.value(7)));
    main_query.where(main_query.import_expression(builder.exists(sub), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"u\" WHERE (\"u\".\"user_id\" = $2))");
}

TEST(Ruvia, db_query_import_expression_preserves_nested_source_shadowing) {
    db_query inner;
    inner.select(inner.value(1))
        .from("orders", "u")
        .where(inner.binary(inner.column("user_id", "u"), op_type::equal, inner.column("id", "users")));
    db_query middle;
    middle.select(middle.column("id", "users")).from("public.users").where(middle.exists(inner));
    db_query builder;
    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(builder.exists(middle), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"public\".\"users\" WHERE EXISTS (SELECT $1 FROM \"orders\" AS \"u\" WHERE (\"u\".\"user_id\" = \"users\".\"id\")))");
}

TEST(Ruvia, db_query_import_expression_maps_nested_correlations_without_crossing_local_scopes) {
    db_query inner;
    inner.select(inner.column("id", "users")).from("public.users");
    db_query middle;
    middle.select(middle.column("id", "users")).from("orders", "o").where(middle.exists(inner));
    db_query builder;
    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(builder.exists(middle), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"public\".\"users\"))");
}

TEST(Ruvia, db_query_import_expression_rejects_capture_by_an_enclosing_subquery) {
    db_query inner;
    inner.select(inner.column("id", "users")).from("items", "i");
    db_query middle;
    middle.select(middle.column("id", "u")).from("orders", "u").where(middle.exists(inner));
    db_query builder;
    const auto expression = builder.exists(middle);
    db_query main_query;
    bool rejected = false;
    try {
        (void)main_query.import_expression(expression, "users", "u");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, db_query_import_expression_keeps_set_operand_scopes_independent) {
    for (const std::string_view qualifier : {"users", "u"}) {
        db_query branch;
        branch.select(branch.column("id", "users")).from("orders", "o");
        db_query sub;
        sub.select(sub.column("id", qualifier));
        if (qualifier == "users") {
            sub.from("public.users");
        } else {
            sub.from("orders", "u");
        }
        sub.combine(db_set_operation::union_all, branch);
        db_query builder;
        db_query main_query;
        main_query.select(main_query.column("id", "u"))
            .from("users", "u")
            .where(main_query.import_expression(builder.exists(sub), "users", "u"));
        const std::string expected(qualifier == "users"
                                       ? "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS ((SELECT \"users\".\"id\" FROM \"public\".\"users\") UNION ALL (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\"))"
                                       : "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS ((SELECT \"u\".\"id\" FROM \"orders\" AS \"u\") UNION ALL (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\"))");
        EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }), expected);
    }
}

TEST(Ruvia, db_query_import_expression_keeps_cte_body_scopes_independent) {
    db_query cte;
    cte.select(cte.column("id", "users")).from("orders", "o");
    db_query sub;
    sub.with("matched", cte).select(sub.column("id", "users")).from("public.users");
    db_query builder;
    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(builder.exists(sub), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (WITH \"matched\" AS (SELECT \"u\".\"id\" FROM \"orders\" AS \"o\") SELECT \"users\".\"id\" FROM \"public\".\"users\")");
}

TEST(Ruvia, db_query_import_expression_preserves_schema_qualified_join_bindings) {
    db_query sub;
    sub.select(sub.column("id", "users"))
        .from("orders", "o")
        .join(db_join_type::inner, "public.users",
            sub.binary(sub.column("user_id", "o"), op_type::equal, sub.column("id", "users")));
    db_query builder;
    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(builder.exists(sub), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"users\".\"id\" FROM \"orders\" AS \"o\" INNER JOIN \"public\".\"users\" ON (\"o\".\"user_id\" = \"users\".\"id\"))");
}

TEST(Ruvia, db_query_import_expression_rejects_qualified_table_and_join_target_capture) {
    for (const bool joined : {false, true}) {
        db_query sub;
        sub.select(sub.column("id", "users"));
        if (joined) {
            sub.from("orders", "o").join(db_join_type::cross, "items", {}, "u");
        } else {
            sub.from("public.u");
        }
        db_query builder;
        const auto expression = builder.exists(sub);
        db_query main_query;
        bool rejected = false;
        try {
            (void)main_query.import_expression(expression, "users", "u");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, db_query_import_expression_rewrites_deep_correlated_references) {
    db_query inner;
    inner.select(inner.column("id", "users")).from("items", "i");
    db_query middle;
    middle.select(middle.column("id", "o")).from("orders", "o").where(middle.exists(inner));
    db_query builder;
    db_query main_query;
    main_query.select(main_query.column("id", "u"))
        .from("users", "u")
        .where(main_query.import_expression(builder.exists(middle), "users", "u"));
    EXPECT_EQ(compile_sql([&] { return main_query.compile(db_driver::postgresql); }),
        "SELECT \"u\".\"id\" FROM \"users\" AS \"u\" WHERE EXISTS (SELECT \"o\".\"id\" FROM \"orders\" AS \"o\" WHERE EXISTS (SELECT \"u\".\"id\" FROM \"items\" AS \"i\"))");
}

TEST(Ruvia, db_query_at_time_zone_and_using_join_render_and_validate) {
    db_query query;
    const auto local_ts = query.at_time_zone(query.column("created_at", "e"), "Asia/Shanghai");
    const auto offset_ts = query.at_time_zone(
        query.column("created_at", "e"),
        query.cast(query.value("08:00:00"), db_data_type::interval));
    query.select({local_ts, offset_ts})
        .from("events", "e")
        .join(db_join_type::left, "hourly_totals", {}, "h", {"hour"})
        .group_by({local_ts, offset_ts});
    const auto statement = query.compile(db_driver::postgresql);
    EXPECT_EQ(statement.sql(),
        "SELECT (\"e\".\"created_at\" AT TIME ZONE $1::text), (\"e\".\"created_at\" AT TIME ZONE CAST($2 AS INTERVAL)) "
        "FROM \"events\" AS \"e\" LEFT JOIN \"hourly_totals\" AS \"h\" USING (\"hour\") "
        "GROUP BY (\"e\".\"created_at\" AT TIME ZONE $1::text), (\"e\".\"created_at\" AT TIME ZONE CAST($2 AS INTERVAL))");
    EXPECT_EQ(statement.params().size(), std::size_t{2});

    const auto literal = query.compile(db_driver::postgresql, db_parameter_mode::literal);
    EXPECT_TRUE(literal.sql().find("(\"e\".\"created_at\" AT TIME ZONE E'Asia/Shanghai')") != std::string_view::npos);
    EXPECT_TRUE(testing::throws_on([&] { (void)query.compile(db_driver::mariadb); }));
}

RUVIA_DB_ENTITY(user_entity, "users",
    db_column<"id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>,
    db_column<"department_id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .nullable_ = true}>,
    db_column<"nickname", std::string, db_column_options{.nullable_ = true}>,
    db_column<"active", bool>,
    db_column<"revision", std::int64_t>,
    db_column<"config", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>,
    db_column<"updated_at", std::string, db_column_options{.data_type_ = db_data_type::timestamp_tz}>)
RUVIA_DB_ENTITY(dept_entity, "departments",
    db_column<"id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>,
    db_column<"name", std::string>)

TEST(Ruvia, db_field_reference_and_predicate_cover_aliases_columns_json_and_owned_assignments) {
    static_assert(user_entity::column_name<"department_id">() == "department_id");
    EXPECT_EQ(user_entity::column<"id">("u").alias(), std::string_view("u"));
    EXPECT_EQ(user_entity::column<"id">().with_alias("u").alias(), std::string_view("u"));
    EXPECT_EQ(user_entity::column<"id">().table_name(), std::string_view("users"));

    db_predicate dynamic_where;
    EXPECT_TRUE(dynamic_where.empty());
    dynamic_where.and_where(user_entity::column<"department_id">("u") == dept_entity::column<"id">("d"))
        .and_where(user_entity::column<"revision">("u").is_distinct_from(0))
        .and_where(user_entity::column<"nickname">("u").not_like("tmp%"))
        .and_where(user_entity::column<"nickname">("u").not_ilike("bot%"))
        .and_where(user_entity::column<"nickname">("u").regex("^[a-z]+$"))
        .and_where(user_entity::column<"nickname">("u").iregex("^[a-z0-9]+$"))
        .and_where(user_entity::column<"revision">("u").not_between(10, 20))
        .and_where(user_entity::column<"revision">("u").not_in({11, 12}))
        .and_where(user_entity::column<"active">("u").is_true())
        .and_where(user_entity::column<"config">("u").json_contains("{\"enabled\":true}"))
        .and_where(user_entity::column<"config">("u").json_has_key("mode"))
        .and_where(user_entity::column<"config">("u").json_has_any_key({"a", "b"}))
        .and_where(user_entity::column<"config">("u").json_has_all_keys({"c", "d"}))
        .and_where(user_entity::column<"config">("u").json_text_equals("env", "prod"))
        .and_where(user_entity::column<"config">("u").json_path_text_equals({"limits", "tier"}, "gold"));

    db_query sub;
    sub.select(sub.column("id")).from("departments");
    dynamic_where.or_where(user_entity::column<"department_id">("u").in(sub) && db_predicate::exists(sub) && db_predicate::not_exists(sub));

    db_query query;
    query.select({
                     user_entity::column<"id">("u").select().expression(query),
                     dept_entity::column<"name">("d").as("dept_name").expression(query),
                     user_entity::column<"nickname">("u").coalesce("anon").expression(query),
                     user_entity::column<"department_id">("u").coalesce_text("").expression(query),
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
    update_query.update("users").set(copy_of_assignments).where(user_entity::column<"id">().expression(update_query));
    const auto update_compiled = update_query.compile(db_driver::postgresql);
    EXPECT_EQ(update_compiled.sql(),
        "UPDATE \"users\" SET \"nickname\" = $1, \"department_id\" = CAST(NULLIF($2::text, $3::text) AS UUID), "
        "\"revision\" = (\"users\".\"revision\" + $4), \"updated_at\" = \"now\"() WHERE \"users\".\"id\"");
}

RUVIA_DB_ENTITY(alert_entity, "alert_state",
    db_column<"device_id", std::string, db_column_options{.data_type_ = db_data_type::uuid, .primary_key_ = true}>,
    db_column<"observed_at_ms", std::int64_t>,
    db_column<"message_id", std::string, db_column_options{.data_type_ = db_data_type::uuid}>,
    db_column<"data", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>,
    db_column<"previous_data", std::string, db_column_options{.data_type_ = db_data_type::jsonb}>,
    db_column<"tags", std::vector<std::string>>)

TEST(Ruvia, db_expr_and_db_tuple_compose_without_external_query) {
    const auto newer_tuple =
        db_tuple{alert_entity::column<"observed_at_ms">().excluded_expr(), alert_entity::column<"message_id">().excluded_expr()} >
        db_tuple{alert_entity::column<"observed_at_ms">(), alert_entity::column<"message_id">()};
    const auto prev_expr = db_expr::case_when(
        {{newer_tuple, alert_entity::column<"data">()}},
        alert_entity::column<"previous_data">());
    const auto json_patched = db_expr::jsonb_set(
        alert_entity::column<"data">(),
        {"meta", "updated"},
        db_expr::jsonb_build_object({db_expr::value("at"), db_expr::now(), db_expr::value("first_tag"), alert_entity::column<"tags">().to_expr()[1]}));
    const auto filtered_agg = db_expr::jsonb_agg(
        alert_entity::column<"device_id">().to_expr().cast(db_data_type::text),
        {alert_entity::column<"observed_at_ms">().to_expr().desc()})
                                  .filter(alert_entity::column<"observed_at_ms">() > 0);
    const db_expr window_partitions[]{alert_entity::column<"device_id">()};
    const db_order_term window_order[]{alert_entity::column<"observed_at_ms">().to_expr().asc()};
    const auto windowed = db_expr::call("lag", {alert_entity::column<"observed_at_ms">()})
                              .over({
                                  .partition_by_ = window_partitions,
                                  .order_by_ = window_order,
                              });
    const auto bracket_where =
        ((alert_entity::column<"observed_at_ms">() > 100) &&
            ((alert_entity::column<"device_id">() == "a") || (alert_entity::column<"device_id">() == "b"))) &&
        db_tuple{alert_entity::column<"device_id">(), alert_entity::column<"message_id">()}.in({
            db_tuple{db_expr::value("a"), db_expr::value("m1")},
            db_tuple{db_expr::value("b"), db_expr::value("m2")},
        });

    db_query query;
    query.select({
                     query.alias(prev_expr.expression(query), "prev"),
                     query.alias(json_patched.expression(query), "patched"),
                     query.alias(filtered_agg.expression(query), "devices"),
                     query.alias(windowed.expression(query), "prev_ms"),
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
            auto col = source.column("nickname", "users");
            auto pred = source.binary(col, db_binary_operator::equal, source.value(std::string(200, 'a')));
            owned_expr = db_expr::from_expression(col, std::string("users"), false);
            aliased_expr = db_expr::from_expression(source.column("nickname", "u"), std::string("u"), true);
            owned_pred = db_predicate::from_expression(pred);
        }

        db_query target;
        target.select({
                          owned_expr.expression(target, {}, "u"),
                          aliased_expr.expression(target, "u", "ignored"),
                      })
            .from("users", "u")
            .where(owned_pred.expression(target, "users", "u"));
        const auto compiled = target.compile(db_driver::postgresql);
        EXPECT_EQ(compiled.sql(),
            R"(SELECT "u"."nickname", "u"."nickname" FROM "users" AS "u" WHERE ("u"."nickname" = $1))");
        EXPECT_EQ(compiled.params().size(), std::size_t{1});

        const auto combined = owned_expr.concat("_suffix");
        db_query combined_target;
        combined_target.select(combined.expression(combined_target, {}, "u")).from("users", "u");
        const auto combined_compiled = combined_target.compile(db_driver::postgresql);
        EXPECT_EQ(combined_compiled.sql(),
            R"(SELECT ("u"."nickname" || $1) FROM "users" AS "u")");

        owned_expr = {};
        aliased_expr = {};
        owned_pred = {};
    }
}

TEST(Ruvia, db_query_self_import_and_tuple_empty_in_and_predicate_self_merge) {
    {
        db_query query;
        const auto a = query.column("id", "src");
        const auto b = query.column("id", "other");
        const auto expression = query.binary(a, db_binary_operator::equal, b);
        (void)query.value(0);
        const auto imported = query.import_expression(expression, "src", "dst");
        query.select(imported).from("items", "dst");
        const auto compiled = query.compile(db_driver::postgresql);
        EXPECT_EQ(
            compiled.sql(), R"(SELECT ("dst"."id" = "other"."id") FROM "items" AS "dst")");
    }
    {
        db_tuple tuple{db_expr::value(1)};
        const auto empty_in = tuple.in({});
        const auto empty_not_in = tuple.not_in({});
        db_query q1;
        q1.select(q1.value(1)).where(empty_in.expression(q1));
        const auto c1 = q1.compile(db_driver::postgresql);
        EXPECT_EQ(c1.sql(), "SELECT $1 WHERE FALSE");
        db_query q2;
        q2.select(q2.value(1)).where(empty_not_in.expression(q2));
        const auto c2 = q2.compile(db_driver::postgresql);
        EXPECT_EQ(c2.sql(), "SELECT $1 WHERE TRUE");
    }
    {
        auto pred = db_expr::column("id") == 1;
        pred.and_where(pred);
        pred.or_where(pred);
        db_query query;
        query.select(query.value(1)).from("items").where(pred.expression(query));
        const auto compiled = query.compile(db_driver::postgresql);
        EXPECT_EQ(compiled.sql(),
            R"(SELECT $1 FROM "items" WHERE ((("id" = $2) AND ("id" = $3)) OR (("id" = $4) AND ("id" = $5))))");
    }
}

RUVIA_DB_PROJECTION(projection_schema_check,
    db_column<"id", std::int64_t>,
    db_column<"label", std::string>)

TEST(Ruvia, db_query_clear_and_bitwise_and_projection_schema) {
    static_assert(projection_schema_check::column_index<"id">() == 0);
    static_assert(projection_schema_check::column_index<"label">() == 1);
    static_assert(projection_schema_check::column_name<"id">() == "id");
    static_assert(projection_schema_check::column_name<"label">() == "label");
    const auto bit_expr = ((db_expr::column("flags") & 3) | 4) ^ 1;
    db_query query;
    query.select(bit_expr.expression(query))
        .from("items")
        .where(query.binary(query.column("id"), db_binary_operator::equal, query.value(1)))
        .group_by({query.column("id")})
        .having(query.binary(query.column("id"), db_binary_operator::greater, query.value(0)));
    EXPECT_TRUE(query.has_where());
    query.clear_where().clear_having();
    EXPECT_TRUE(!query.has_where());
    auto compiled = query.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), R"(SELECT ((("flags" & $1) | $2) # $3) FROM "items" GROUP BY "id")");

    db_query other;
    other.select(other.value(42));
    compiled = other.compile(db_driver::postgresql);
    EXPECT_EQ(compiled.sql(), "SELECT $1");
    EXPECT_EQ(compiled.params().size(), std::size_t{1});
}

TEST(Ruvia, db_query_postgresql_numbers_each_parameter_node_once_and_caps_the_count) {
    // Equal grouped expressions imported separately into SELECT and GROUP BY
    // share their numbers, beyond any small fixed table of reused nodes.
    db_query grouped;
    std::vector<db_expression> projections{};
    std::vector<db_expression> groups{};
    for (int bucket = 0; bucket < 40; ++bucket) {
        const auto expression = db_expr::column("id") + bucket;
        projections.push_back(expression.expression(grouped));
        groups.push_back(expression.expression(grouped));
    }
    grouped.select(projections).from("events").group_by(groups);
    const auto statement = grouped.compile(db_driver::postgresql);
    EXPECT_EQ(statement.params().size(), std::size_t{40});
    EXPECT_TRUE(statement.sql().find("FROM \"events\" GROUP BY (\"id\" + $1), (\"id\" + $2), ") != std::string_view::npos);
    EXPECT_TRUE(statement.sql().ends_with("(\"id\" + $40)"));

    // The PostgreSQL Bind message counts parameters in an Int16.
    db_query bulk;
    bulk.insert_into("events", {"id"});
    std::vector<db_expression> values{};
    values.reserve(65536);
    for (std::int64_t id = 0; id < 65536; ++id) {
        values.push_back(bulk.value(id));
    }
    const std::string_view columns[]{"id"};
    const std::span<const db_expression> all_values(values);
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
    const auto tenant = source.binary(source.column("tenant_id", "orders"), op_type::equal, source.value(7));
    const auto owned = db_expr::from_expression(tenant, "orders");
    const db_expr borrowed(tenant, "orders");
    for (const auto* expression : {&owned, &borrowed}) {
        db_query users;
        users.select(users.star("u")).from("users", "u").where(expression->expression(users, "users", "u"));
        EXPECT_EQ(compile_sql([&] { return users.compile(db_driver::postgresql); }),
            R"(SELECT "u".* FROM "users" AS "u" WHERE ("orders"."tenant_id" = $1))");
        // Without a caller table, the stored table names the source.
        db_query orders;
        orders.select(orders.star("o")).from("orders", "o").where(expression->expression(orders, {}, "o"));
        EXPECT_EQ(compile_sql([&] { return orders.compile(db_driver::postgresql); }),
            R"(SELECT "o".* FROM "orders" AS "o" WHERE ("o"."tenant_id" = $1))");
    }
}

TEST(Ruvia, db_query_statement_kind_change_rejects_clauses_that_would_widen_the_target) {
    db_query query;
    query.from("users", "u").where(query.binary(query.column("id", "u"), op_type::equal, query.value(1)));
    EXPECT_TRUE(testing::throws_on([&] { query.delete_from("users"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.update("users"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.insert_into("users"); }));
    EXPECT_EQ(compile_sql([&] { return query.compile(db_driver::postgresql); }),
        R"(SELECT * FROM "users" AS "u" WHERE ("u"."id" = $1))");

    db_query projected;
    projected.select(projected.value(1));
    EXPECT_TRUE(testing::throws_on([&] { projected.update("users"); }));

    // WHERE carries over; re-targeting the same kind and the explicit USING
    // spelling stay available.
    db_query filtered;
    filtered.where(filtered.binary(filtered.column("id", "a"), op_type::equal, filtered.value(1))).delete_from("users");
    filtered.delete_from("accounts", "a").delete_using("users", "u");
    EXPECT_EQ(compile_sql([&] { return filtered.compile(db_driver::postgresql); }),
        R"(DELETE FROM "accounts" AS "a" USING "users" AS "u" WHERE ("a"."id" = $1))");
    EXPECT_TRUE(testing::throws_on([&] { filtered.update("accounts"); }));
}

static_assert(!std::is_constructible_v<db_expr, db_expression, std::string>);
static_assert(std::is_constructible_v<db_expr, db_expression, const std::string&>);
static_assert(std::is_constructible_v<db_expr, db_expression, std::string_view>);

TEST(Ruvia, db_query_raw_sql_keeps_comment_tokens_and_positional_parameters_out) {
    db_query query;
    const auto difference = query.sql({"(10 -", ")"}, {query.value(-5)});
    const auto quotient = query.sql({"10 /", ""}, {query.star()});
    for (const auto driver : {db_driver::postgresql, db_driver::mariadb}) {
        EXPECT_EQ(db_query::render_expression(difference, driver), "((10 - -5))");
        EXPECT_EQ(db_query::render_expression(quotient, driver), "(10 / *)");
    }

    db_query positional;
    positional.select(positional.sql({"$1 + ", ""}, {positional.value(1)}));
    EXPECT_TRUE(testing::throws_on([&] { (void)positional.compile(db_driver::postgresql); }));
    db_query trailing;
    trailing.select(trailing.sql({"", " = $2"}, {trailing.value(1)}));
    EXPECT_TRUE(testing::throws_on([&] { (void)trailing.compile(db_driver::postgresql); }));
    // Literal output (SQL function bodies) and MariaDB do not number `$n`.
    EXPECT_EQ(compile_sql([&] { return positional.compile(db_driver::postgresql, db_parameter_mode::literal); }),
        "SELECT ($1 + 1)");
    EXPECT_EQ(compile_sql([&] { return positional.compile(db_driver::mariadb); }), "SELECT ($1 + ?)");
    // A `$` that continues an identifier is not a parameter.
    db_query identifier;
    identifier.select(identifier.sql("price$1")).from("items");
    EXPECT_EQ(compile_sql([&] { return identifier.compile(db_driver::postgresql); }),
        R"(SELECT (price$1) FROM "items")");
}

TEST(Ruvia, db_query_rejects_dotted_aliases_cte_names_and_lock_targets) {
    db_query query;
    db_query body;
    body.select(body.value(1));
    const borrowed_text dotted_tables[]{"u.v"};
    EXPECT_TRUE(testing::throws_on([&] { query.from("users", "u.v"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.from(body, "s.t"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.join(db_join_type::cross, "accounts", {}, "a.b"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.join(db_join_type::cross, body, {}, "a.b"); }));
    EXPECT_TRUE(testing::throws_on([&] { query.with("w.x", body); }));
    EXPECT_TRUE(testing::throws_on([&] { query.lock({.tables_ = dotted_tables}); }));
    EXPECT_TRUE(testing::throws_on([&] { db_query().update("users", "u.v"); }));
    EXPECT_TRUE(testing::throws_on([&] { db_query().delete_from("users", "u.v"); }));
    EXPECT_TRUE(testing::throws_on([&] { db_query().insert_into("users", {}, "u.v"); }));

    // Schema-qualified tables keep working with single-identifier aliases.
    const borrowed_text lock_tables[]{"u"};
    query.from("app.users", "u").lock({.tables_ = lock_tables});
    EXPECT_EQ(compile_sql([&] { return query.compile(db_driver::postgresql); }),
        R"(SELECT * FROM "app"."users" AS "u" FOR UPDATE OF "u")");
}

TEST(Ruvia, db_query_and_or_chains_are_one_nesting_level) {
    // 300 conjuncts would exceed the 256-level limit if every AND nested.
    db_predicate filter;
    db_query built;
    db_query correlated;
    correlated.select(correlated.value(1)).from("events", "e");
    for (int index = 0; index < 300; ++index) {
        filter.and_where(db_expr::column("id") != index);
        built.and_where(built.binary(built.column("id"), op_type::not_equal, built.value(index)));
        correlated.and_where(correlated.binary(correlated.column("owner", "src"), op_type::not_equal, correlated.value(index)));
    }
    db_query owned;
    owned.select(owned.star()).from("items").where(filter.expression(owned));
    const auto owned_statement = owned.compile(db_driver::postgresql);
    EXPECT_EQ(owned_statement.params().size(), std::size_t{300});
    EXPECT_TRUE(owned_statement.sql().starts_with(R"(SELECT * FROM "items" WHERE (("id" <> $1) AND ("id" <> $2) AND ("id" <> $3) AND )"));
    EXPECT_TRUE(owned_statement.sql().ends_with(R"( AND ("id" <> $300)))"));

    built.select(built.star()).from("items");
    EXPECT_EQ(compile_sql([&] { return built.clone().compile(db_driver::postgresql); }),
        std::string(owned_statement.sql()));

    // Importing with a qualifier mapping walks the chain inside a correlated subquery too.
    db_query source;
    const auto exists = source.exists(correlated);
    db_query target;
    target.select(target.star()).from("items", "dst").where(target.import_expression(exists, "src", "dst"));
    const auto target_sql = compile_sql([&] { return target.compile(db_driver::postgresql); });
    EXPECT_TRUE(target_sql.find(R"(WHERE EXISTS (SELECT $1 FROM "events" AS "e" WHERE (("dst"."owner" <> $2) AND ("dst"."owner" <> $3) AND )") != std::string::npos);
    EXPECT_TRUE(target_sql.find(R"("src")") == std::string::npos);
}

TEST(Ruvia, db_query_rvalue_composition_matches_copying_and_consumes_the_argument) {
    const auto make_source = [] {
        db_query source;
        source.select(source.value(1)).from("events");
        return source;
    };
    db_query copied;
    const auto copied_source = make_source();
    copied.select(copied.star()).from(copied_source, "s").where(copied.exists(copied_source));
    const auto expected = compile_sql([&] { return copied.compile(db_driver::postgresql); });

    db_query moved;
    auto adopted = make_source();
    auto foreign = make_source();
    moved.select(moved.star()).from(std::move(adopted), "s").where(moved.exists(std::move(foreign)));
    EXPECT_EQ(compile_sql([&] { return moved.compile(db_driver::postgresql); }), expected);
    EXPECT_TRUE(testing::throws_on([&] { (void)adopted.compile(db_driver::postgresql); }));
    EXPECT_TRUE(testing::throws_on([&] { (void)foreign.compile(db_driver::postgresql); }));
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
        initial.select(initial.value(7));
        target.insert_into("items", {"id"}).insert_from(std::move(initial));
    } else {
        target.select(target.value(9)).from("items");
    }
    return target;
}

template <typename query_type>
std::optional<db_expression> compose_query(db_query& target, query_type&& source, query_composition operation) {
    constexpr std::string_view alias = "nested_query_with_a_long_alias";
    constexpr std::array<borrowed_text, 1> columns{"nested_column_with_a_long_name"};
    switch (operation) {
        case query_composition::scalar:
            return target.subquery(std::forward<query_type>(source));
        case query_composition::exists:
            return target.exists(std::forward<query_type>(source));
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
    for (const auto operation : {query_composition::scalar, query_composition::exists,
             query_composition::source, query_composition::join, query_composition::insert,
             query_composition::cte, query_composition::combine}) {
        SCOPED_TRACE(static_cast<int>(operation));
        auto target = composition_target(operation);
        db_query source;
        source.select(source.value(17)).from("source_rows");
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
        EXPECT_THROW((void)source.returns_rows(), std::logic_error);
        EXPECT_EQ(compile_sql([&] {
            return target.compile(db_driver::postgresql, db_parameter_mode::literal);
        }),
            expected_sql);
    }
    for (const auto operation : {query_composition::scalar, query_composition::exists,
             query_composition::source, query_composition::join,
             query_composition::cte, query_composition::combine}) {
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
    EXPECT_EQ(maria_literal.sql(),
        "SELECT * FROM `device` LIMIT 9223372036854775808 OFFSET 18446744073709551615");
    EXPECT_EQ(maria_literal.sql().find('-'), std::string_view::npos);
}

}  // namespace

#if defined(RUVIA_ENABLE_POSTGRESQL) && defined(RUVIA_ENABLE_REDIS)

// Removing a query-result cache id must drop every result a repository builder
// cached under that id, so the next get_one/get_exists/get_count reads the
// database again.

namespace {

RUVIA_DB_ENTITY(record, "records", ruvia::db_column<"v", std::string>)

constexpr std::string_view cache_id = "item";
constexpr std::chrono::milliseconds cache_lifetime = std::chrono::minutes(1);
// What every cache entry starts with, as the framework writes it.
constexpr std::string_view cache_entry_magic = "RUVIAQC";

// A RESP2 key-value store answering GET, SET, DEL and prefix SCAN on every accepted
// connection. Without credentials or a database index the client sends no
// handshake. All connections run on one thread, so the map needs no lock.
class redis_store_peer final {
public:
    redis_store_peer()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          work_(asio::make_work_guard(io_)),
          thread_([this] { io_.run(); }) {
        asio::co_spawn(io_, accept_connections(), [this](std::exception_ptr failure) { record_failure(failure); });
    }

    redis_store_peer(const redis_store_peer&) = delete;
    redis_store_peer& operator=(const redis_store_peer&) = delete;

    ~redis_store_peer() {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            work_.reset();
        });
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    // Every later GET answers `value`, whatever the key, until the peer ends.
    void poison_reads(std::string_view value) {
        std::promise<void> poisoned;
        auto result = poisoned.get_future();
        asio::post(io_, [this, &poisoned, value = std::string(value)]() mutable {
            poison_ = std::move(value);
            poisoned.set_value();
        });
        result.get();
    }

    void stall_next_read(std::function<void()> on_read) {
        std::promise<void> configured;
        auto ready = configured.get_future();
        asio::post(io_, [this, &configured, on_read = std::move(on_read)]() mutable {
            on_read_ = std::move(on_read);
            configured.set_value();
        });
        ready.get();
    }

    // Rethrows the first connection failure instead of reporting a count.
    [[nodiscard]] std::size_t key_count() {
        std::promise<std::size_t> counted;
        auto result = counted.get_future();
        asio::post(io_, [this, &counted] {
            if (failure_) {
                counted.set_exception(failure_);
            } else {
                counted.set_value(store_.size());
            }
        });
        return result.get();
    }

private:
    using socket_type = asio::ip::tcp::socket;

    void record_failure(std::exception_ptr failure) noexcept {
        if (failure && !failure_) {
            failure_ = failure;
        }
    }

    [[nodiscard]] bool closed(const std::error_code& error) const noexcept {
        return error == asio::error::eof || error == asio::error::connection_reset ||
               error == asio::error::connection_aborted ||
               (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept_connections() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) { record_failure(failure); });
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    static asio::awaitable<std::string> read_line(socket_type& socket, std::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        std::string line(std::string_view(buffer).substr(0, size - 2));
        buffer.erase(0, size);
        co_return line;
    }

    static asio::awaitable<std::vector<std::string>> read_command(socket_type& socket, std::string& buffer) {
        const auto header = co_await read_line(socket, buffer);
        if (header.size() < 2 || header.front() != '*') {
            throw std::runtime_error("invalid RESP command array");
        }
        const auto count = std::stoull(std::string(std::string_view(header).substr(1)));
        std::vector<std::string> arguments{};
        arguments.reserve(count);
        for (std::size_t index = 0; index != count; ++index) {
            const auto length_line = co_await read_line(socket, buffer);
            if (length_line.size() < 2 || length_line.front() != '$') {
                throw std::runtime_error("invalid RESP bulk string");
            }
            const auto length = static_cast<std::size_t>(std::stoull(std::string(std::string_view(length_line).substr(1))));
            if (buffer.size() < length + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(length + 2 - buffer.size()), asio::use_awaitable);
            }
            if (buffer.compare(length, 2, "\r\n") != 0) {
                throw std::runtime_error("invalid RESP bulk ending");
            }
            arguments.emplace_back(std::string_view(buffer).substr(0, length));
            buffer.erase(0, length + 2);
        }
        co_return arguments;
    }

    // Expiration options after a SET value are accepted and not enforced; the
    // scenario completes well within the cache lifetime.
    [[nodiscard]] std::string execute(std::vector<std::string>& arguments) {
        if (arguments.empty()) {
            return std::string("-ERR empty command\r\n");
        }
        auto& name = arguments.front();
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::toupper(value));
        });
        if (name == "GET" && arguments.size() == 2) {
            if (poison_) {
                return "$" + std::string(std::to_string(poison_->size())) + "\r\n" + *poison_ + "\r\n";
            }
            const auto found = store_.find(arguments[1]);
            if (found == store_.end()) {
                return std::string("$-1\r\n");
            }
            return "$" + std::string(std::to_string(found->second.size())) + std::string("\r\n") + found->second + "\r\n";
        }
        if (name == "SET" && arguments.size() >= 3) {
            store_.insert_or_assign(std::move(arguments[1]), std::move(arguments[2]));
            return std::string("+OK\r\n");
        }
        if (name == "DEL" && arguments.size() >= 2) {
            std::size_t removed = 0;
            for (std::size_t index = 1; index != arguments.size(); ++index) {
                removed += store_.erase(arguments[index]);
            }
            return ":" + std::string(std::to_string(removed)) + "\r\n";
        }
        if (name == "SCAN" && arguments.size() == 6 && arguments[1] == "0" &&
            arguments[2] == "MATCH" && arguments[3].ends_with('*') && arguments[4] == "COUNT") {
            const auto prefix = std::string_view(arguments[3]).substr(0, arguments[3].size() - 1);
            std::string values{};
            std::size_t count = 0;
            for (const auto& [key, value] : store_) {
                (void)value;
                if (key.starts_with(prefix)) {
                    values.append("$").append(std::string(std::to_string(key.size()))).append("\r\n").append(key).append("\r\n");
                    ++count;
                }
            }
            return "*2\r\n$1\r\n0\r\n*" + std::string(std::to_string(count)) + std::string("\r\n") + values;
        }
        return std::string("-ERR unsupported command\r\n");
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::string buffer{};
        try {
            for (;;) {
                auto arguments = co_await read_command(*socket, buffer);
                if (!arguments.empty() && arguments.front() == "GET" && on_read_) {
                    auto on_read = std::exchange(on_read_, {});
                    on_read();
                    continue;
                }
                const auto reply = execute(arguments);
                co_await asio::async_write(*socket, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::vector<std::shared_ptr<socket_type>> sockets_{};
    std::map<std::string, std::string, std::less<>> store_{};
    std::optional<std::string> poison_;
    std::function<void()> on_read_;
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

// Every read names the same cache id; each result shape stores its own entry.
ruvia::task<std::string> read_one(const ruvia::db_session& handle) {
    const auto row =
        co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_one();
    co_return row ? std::string(std::string_view(row->get<"v">())) : std::string("missing");
}

ruvia::task<bool> read_exists(const ruvia::db_session& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_exists();
}

ruvia::task<std::uint64_t> read_count(const ruvia::db_session& handle) {
    co_return co_await handle.repository<record>().query_builder().cache(cache_id, cache_lifetime).get_count();
}

ruvia::task<std::vector<std::string>> read_many(const ruvia::db_session& handle, std::string_view id = cache_id) {
    const auto rows =
        co_await handle.repository<record>().query_builder().cache(id, cache_lifetime).get_many();
    std::vector<std::string> values{};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        values.emplace_back(std::string_view(rows[index].get<"v">()));
    }
    co_return values;
}

ruvia::task<std::string> read_sql_cached(const ruvia::db_session& handle, std::string_view parameter) {
    auto query = handle.repository<record>().query_builder();
    query.where(record::column<"v">() == parameter).cache(cache_lifetime);
    const auto row = co_await query.get_one();
    co_return row ? std::string(std::string_view(row->get<"v">())) : std::string("missing");
}

}  // namespace

namespace query_cache_test {

// What one concurrent cached read observed. Failures are recorded rather than
// thrown so every reader in a scope finishes on its own.
struct read_outcome final {
    std::string value_{};
    std::optional<ruvia::db_error::code_type> error_;
    bool done_{false};
};

ruvia::task<void> read_sql_into(ruvia::db_session handle, std::string_view parameter, read_outcome& outcome) {
    try {
        outcome.value_ = co_await read_sql_cached(handle, parameter);
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

ruvia::task<void> read_many_into(ruvia::db_session handle, read_outcome& outcome) {
    try {
        const auto values = co_await read_many(handle);
        outcome.value_ = values.empty() ? std::string("missing") : values.front();
    } catch (const ruvia::db_error& error) {
        outcome.error_ = error.code();
    }
    outcome.done_ = true;
}

// Waits on the worker, a millisecond at a time, until `ready` holds.
template <typename predicate_type>
ruvia::task<void> wait_until(const ruvia::worker_ref& worker, predicate_type ready) {
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > give_up) {
            throw std::runtime_error("query cache test condition was not reached");
        }
        (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(1));
    }
}

// Runs `scenario` with a connected Redis client and a database client that
// caches through it, on one event loop; both shut down before a failure is
// reported.
template <typename scenario_type>
void run_with_query_cache(redis_store_peer& redis_peer, const ruvia::db_config& database_config,
    scenario_type& scenario, std::optional<std::size_t> max_reply_bytes = ruvia::redis_config{}.max_reply_bytes_) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    redis_config.max_reply_bytes_ = max_reply_bytes;
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(attachment.loop(), database_config, store, ruvia::db_cache_config{});
            try {
                co_await database.connect();
                co_await scenario(database, store);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

}  // namespace query_cache_test

TEST(Ruvia, db_query_result_cache_remove_invalidates_repository_result_shapes) {
    redis_store_peer redis_peer;
    // The database answers the statements the cache lets through, in order. A
    // read served from the cache consumes no reply, so a stale entry shows up
    // as the previous value and an unexpected database read as the next one.
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({
            {.rows_ = std::vector<std::string>({std::string("one"), std::string("two"), std::string("three")})},
            {.rows_ = std::vector<std::string>({std::string("alpha")})},
            {.column_ = std::string("exists"), .rows_ = std::vector<std::string>({std::string("t")})},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("3")})},
            {.rows_ = std::vector<std::string>({std::string("gamma")})},
            {.rows_ = std::vector<std::string>({std::string("beta")})},
            {.column_ = std::string("exists"), .rows_ = std::vector<std::string>({std::string("f")})},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("5")})},
            {.rows_ = std::vector<std::string>({std::string("delta")})},
            {.rows_ = std::vector<std::string>({std::string("epsilon")})},
        }));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{});
            try {
                co_await database.connect();
                const auto handle = database.db();
                // First reads come from the database and fill the cache.
                const std::vector<std::string> many({std::string("one"), std::string("two"), std::string("three")});
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::string("alpha"));
                EXPECT_TRUE(co_await read_exists(handle));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{3});
                EXPECT_TRUE(redis_peer.key_count() != 0);
                // Repeated reads are served from the cache; every decoded row
                // resolves its column through the result's shared names.
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(co_await read_one(handle), std::string("alpha"));
                EXPECT_TRUE(co_await read_exists(handle));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{3});
                auto raw_query = handle.repository<record>().query_builder();
                raw_query.cache(cache_id, cache_lifetime);
                auto cached_rows = co_await raw_query.get_raw_many();
                auto moved_rows = std::move(cached_rows);
                // Subsequent Redis reads must not invalidate an earlier result.
                EXPECT_TRUE(co_await read_many(handle) == many);
                EXPECT_EQ(moved_rows.size(), many.size());
                for (std::size_t index = 0; index < many.size(); ++index) {
                    EXPECT_TRUE(moved_rows[index]["v"].value() == many[index]);
                }
                // A user id that spells another id plus a shape name is its own
                // entry: it reads the database instead of the "one" result.
                constexpr std::string_view suffixed_id = "item-one";
                const std::vector<std::string> gamma({std::string("gamma")});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // Removing the caller's id drops every result shape cached under
                // it and leaves the independent id's entry alone.
                co_await handle.query_result_cache().remove(cache_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                EXPECT_EQ(co_await read_one(handle), std::string("beta"));
                EXPECT_TRUE(!(co_await read_exists(handle)));
                EXPECT_EQ(co_await read_count(handle), std::uint64_t{5});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == gamma);
                // A multi-id removal drops every shape for each id.
                co_await handle.query_result_cache().remove(cache_id, suffixed_id);
                EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::vector<std::string>({std::string("delta")}));
                // Clear uses the same cached namespace prefix without touching
                // an unrelated Redis key.
                (void)co_await store.set("outside-cache", "keep");
                co_await handle.query_result_cache().clear();
                EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
                const auto outside = co_await store.get("outside-cache");
                EXPECT_TRUE(outside.has_value() && *outside == "keep");
                EXPECT_TRUE(co_await read_many(handle, suffixed_id) == std::vector<std::string>({std::string("epsilon")}));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_query_result_cache_distinguishes_sql_parameter_values) {
    redis_store_peer redis_peer;
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")})}, {.rows_ = std::vector<std::string>({std::string("second")})}}));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                const auto handle = database.db();
                const std::string large_parameter(8192, 'x');
                std::string different_parameter(large_parameter);
                different_parameter.back() = 'y';
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
                EXPECT_EQ(co_await read_sql_cached(handle, different_parameter), std::string("second"));
                EXPECT_EQ(co_await read_sql_cached(handle, large_parameter), std::string("first"));
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_query_result_cache_preserves_empty_and_zero_column_results) {
    redis_store_peer redis_peer;
    // The entry magic, then little-endian 64-bit row and column counts.
    std::string empty(cache_entry_magic);
    const auto row_count_offset = cache_entry_magic.size();
    const auto column_count_offset = row_count_offset + 8;
    empty.append(16, '\0');
    redis_peer.poison_reads(empty);
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>());
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                auto query = database.db().repository<record>().query_builder();
                query.cache(cache_id, cache_lifetime);
                auto empty_rows = co_await query.get_raw_many();
                EXPECT_TRUE(empty_rows.empty());
                auto empty_with_schema = decltype(empty)(empty);
                empty_with_schema[column_count_offset] = 1;
                empty_with_schema.push_back('\1');
                empty_with_schema.append(7, '\0');
                empty_with_schema.push_back('v');
                redis_peer.poison_reads(std::move(empty_with_schema));
                auto named_empty_rows = co_await query.get_raw_many();
                EXPECT_TRUE(named_empty_rows.empty());
                auto zero_columns = decltype(empty)(empty);
                zero_columns[row_count_offset] = 2;
                zero_columns.append(2, '\2');
                redis_peer.poison_reads(std::move(zero_columns));
                auto rows = co_await query.get_raw_many();
                EXPECT_EQ(rows.size(), 2u);
                EXPECT_TRUE(rows[0].empty());
                EXPECT_TRUE(rows[1].empty());
                EXPECT_TRUE(empty_rows.empty());
                EXPECT_TRUE(named_empty_rows.empty());
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

// A corrupt entry claiming an enormous count of zero-column rows is an invalid
// cache entry: it is rejected before any row storage is reserved, so a cache
// that ignores errors reads the database instead.
TEST(Ruvia, db_query_result_cache_rejects_corrupt_zero_column_entries) {
    redis_store_peer redis_peer;
    std::string corrupt(cache_entry_magic);
    corrupt.append(8, '\xff');
    corrupt.append(8, '\0');
    redis_peer.poison_reads(std::move(corrupt));
    ruvia::test::backend_startup_peer database_peer(ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle, std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("fresh")})}}));
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            const auto store = redis.redis();
            ruvia::db_client database(
                attachment.loop(), database_peer.config(), store, ruvia::db_cache_config{.ignore_errors_ = true});
            try {
                co_await database.connect();
                const std::vector<std::string> fresh({std::string("fresh")});
                EXPECT_TRUE(co_await read_many(database.db()) == fresh);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

TEST(Ruvia, db_client_shutdown_cancels_inflight_cache_without_closing_borrowed_redis) {
    redis_store_peer redis_peer;
    ruvia::test::backend_startup_peer database_peer(
        ruvia::test::backend_startup_peer::protocol::postgresql, false,
        ruvia::test::backend_startup_peer::after_startup::idle);
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const ruvia::redis_config redis_config{.host_ = "127.0.0.1",
        .port_ = redis_peer.port(),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .pool_size_per_worker_ = 1};
    auto exercise = [&]() -> ruvia::task<void> {
        ruvia::redis_client redis(attachment.loop(), redis_config);
        std::exception_ptr failure;
        try {
            co_await redis.connect();
            ruvia::db_client database(attachment.loop(), database_peer.config(), redis.redis(), ruvia::db_cache_config{});
            try {
                co_await database.connect();
                redis_peer.stall_next_read([&database] { database.close(); });
                bool cancelled = false;
                try {
                    (void)co_await read_sql_cached(database.db(), "cancel-cache");
                } catch (const ruvia::redis_error& error) {
                    cancelled = error.code() == ruvia::redis_error::code_type::cancelled ||
                                error.code() == ruvia::redis_error::code_type::closing;
                }
                EXPECT_TRUE(cancelled);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await database.shutdown();
            EXPECT_TRUE(!(co_await redis.redis().get("still-open")));
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        co_await redis.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    try {
        ruvia::test::drive_backend_task(attachment, exercise());
    } catch (...) {
        ruvia::test::retire_backend_attachment(attachment);
        throw;
    }
    ruvia::test::retire_backend_attachment(attachment);
}

// An entry whose GET reply would exceed the Redis reply limit is never
// written: the result is returned uncached, and the next read of that key goes
// to the database instead of failing on an oversized reply. Smaller entries
// are still cached.
TEST(Ruvia, db_query_result_cache_skips_entries_beyond_the_redis_reply_limit) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    const std::string large(512, 'x');
    const std::string larger(513, 'y');
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string(large)})}, {.rows_ = std::vector<std::string>({std::string(larger)})}, {.rows_ = std::vector<std::string>({std::string("small")})}}));
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session&) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), large);
        EXPECT_EQ(redis_peer.key_count(), std::size_t{0});
        EXPECT_EQ(co_await read_sql_cached(handle, "large"), larger);
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::string("small"));
        EXPECT_EQ(co_await read_sql_cached(handle, "small"), std::string("small"));
        EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario, std::size_t{256});
}

// Concurrent misses of one key on one worker run the statement once: the
// first reader queries the database and the others take its result.
TEST(Ruvia, db_query_result_cache_coalesces_concurrent_misses_of_one_key) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")}), .deferred_ = true}}));
    std::array<query_cache_test::read_outcome, 3> outcomes;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        for (auto& outcome : outcomes) {
            readers.spawn(query_cache_test::read_sql_into(handle, "same", outcome));
        }
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        // Redis answers in order on its one connection: once this reply is in,
        // every reader has seen its miss.
        (void)co_await store.get("barrier");
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    for (const auto& outcome : outcomes) {
        EXPECT_TRUE(!outcome.error_.has_value());
        EXPECT_TRUE(outcome.value_ == "first");
    }
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{1});
    EXPECT_EQ(redis_peer.key_count(), std::size_t{1});
}

// A cancelled leader ends its fill without a result: the reader waiting for it
// reads the cache again and runs the statement itself. The leader's abandoned
// statement takes its connection with it, so the takeover connects anew.
TEST(Ruvia, db_query_result_cache_waiter_takes_over_from_a_cancelled_leader) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::reaccept,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("abandoned")}), .deferred_ = true}, {.rows_ = std::vector<std::string>({std::string("taken over")})}}));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::stop_source stop;
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_sql_into(
            handle.with_options({.timeout_ = std::chrono::seconds(5), .stop_token_ = stop.token()}), "same", leader));
        readers.spawn(query_cache_test::read_sql_into(handle, "same", waiter));
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        (void)co_await store.get("barrier");
        stop.request_stop();
        co_await query_cache_test::wait_until(database.worker(), [&] { return leader.done_; });
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(leader.error_ == ruvia::db_error::code_type::cancelled);
    EXPECT_TRUE(!waiter.error_.has_value());
    EXPECT_TRUE(waiter.value_ == "taken over");
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{2});
    EXPECT_EQ(database_peer.startups(), std::size_t{2});
}

// Waiting for another reader's fill is bounded by the waiter's own timeout,
// whatever the leader's.
TEST(Ruvia, db_query_result_cache_waiter_is_bounded_by_its_own_timeout) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("first")}), .deferred_ = true}}));
    query_cache_test::read_outcome leader;
    query_cache_test::read_outcome waiter;
    bool waiter_finished_first = false;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session&) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_sql_into(handle, "same", leader));
        readers.spawn(query_cache_test::read_sql_into(
            handle.with_options({.timeout_ = std::chrono::milliseconds(100)}), "same", waiter));
        co_await query_cache_test::wait_until(database.worker(), [&] { return waiter.done_; });
        waiter_finished_first = !leader.done_;
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(waiter_finished_first);
    EXPECT_TRUE(waiter.error_ == ruvia::db_error::code_type::timeout);
    EXPECT_TRUE(!leader.error_.has_value());
    EXPECT_TRUE(leader.value_ == "first");
    EXPECT_EQ(database_peer.postgres_parses().size(), std::size_t{1});
}

// remove() detaches the fills of its keys on this worker: a reader arriving
// after it runs its own statement instead of taking the result of a fill that
// started before the removal.
TEST(Ruvia, db_query_result_cache_remove_detaches_inflight_fills) {
    using peer_type = ruvia::test::backend_startup_peer;
    redis_store_peer redis_peer;
    peer_type database_peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("stale")}), .deferred_ = true}, {.rows_ = std::vector<std::string>({std::string("fresh")})}}));
    query_cache_test::read_outcome before;
    query_cache_test::read_outcome after;
    auto scenario = [&](ruvia::db_client& database, const ruvia::redis_session& store) -> ruvia::task<void> {
        const auto handle = database.db().with_options({.timeout_ = std::chrono::seconds(5)});
        ruvia::task_scope readers(database.worker());
        readers.spawn(query_cache_test::read_many_into(handle, before));
        co_await query_cache_test::wait_until(
            database.worker(), [&] { return database_peer.postgres_parses().size() == 1; });
        co_await handle.query_result_cache().remove(cache_id);
        readers.spawn(query_cache_test::read_many_into(handle, after));
        (void)co_await store.get("barrier");
        database_peer.release_replies();
        co_await readers.join();
    };
    query_cache_test::run_with_query_cache(redis_peer, database_peer.config(), scenario);
    EXPECT_TRUE(!before.error_.has_value());
    EXPECT_TRUE(before.value_ == "stale");
    EXPECT_TRUE(!after.error_.has_value());
    EXPECT_TRUE(after.value_ == "fresh");
}

#endif

using ruvia::db_data_type;
using ruvia::db_driver;
using ruvia::db_generated_type;
using ruvia::db_procedure;
using ruvia::db_schema;
using ruvia::db_schema_column;
using ruvia::db_schema_constraint;
using ruvia::db_table_definition;
using ruvia::db_table_option;
using ruvia::test::throws_on;

RUVIA_DB_ENTITY(enum_entity_type, "enum_events",
    ruvia::db_column<"state", std::string,
        ruvia::db_column_options{.enum_name_ = ruvia::fixed_string{"app.event_state"}, .default_expression_ = ruvia::fixed_string{"'pending'::app.event_state"}}>,
    ruvia::db_column<"created_at", std::string,
        ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"now()"}}>)

TEST(Ruvia, db_schema_entity_named_enum_and_default_expressions) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<enum_entity_type>();
    const auto migrations = schema.compile("enum_defaults");
    EXPECT_TRUE(migrations[0].sql().find("\"state\" \"app\".\"event_state\" NOT NULL DEFAULT ('pending'::app.event_state)") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DEFAULT (now())") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_table<enum_entity_type>(); }));
    ruvia::db_query defaults;
    EXPECT_TRUE(throws_on([&] { schema.create_table<enum_entity_type>({.defaults_ = {{"state", defaults.value("ready")}}}); }));
}

RUVIA_DB_ENTITY(schema_target_type, "schema_targets", ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>)
RUVIA_DB_ENTITY(schema_owner_type, "schema_owners",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"parent_id", std::int64_t>, ruvia::db_column<"parent_tenant_id", std::int64_t>,
    ruvia::db_column<"peer_id", std::int64_t>,
    ruvia::db_many_to_one<"parent", schema_target_type, ruvia::db_join_column<"parent_id", "id">,
        ruvia::db_join_column<"parent_tenant_id", "tenant_id">>,
    ruvia::db_one_to_one<"peer", schema_target_type, ruvia::db_join_column<"peer_id", "id">>,
    ruvia::db_one_to_many<"children", schema_target_type, "parent">,
    ruvia::db_many_to_many<"labels", schema_target_type, ruvia::db_join_table<"owner_labels", ruvia::db_join_columns<ruvia::db_join_column<"owner_id", "id">>, ruvia::db_join_columns<ruvia::db_join_column<"label_id", "id">, ruvia::db_join_column<"label_tenant_id", "tenant_id">>>>)

TEST(Ruvia, db_schema_create_table_composite_foreign_key_and_nullable_default) {
    db_schema schema({.driver_ = db_driver::postgresql});
    db_table_definition table_value{.name_ = "tenant_users", .columns_ = {db_schema_column{.name_ = "tenant_id", .type_ = {.data_type_ = db_data_type::big_int}}, db_schema_column{.name_ = "user_id", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true}}, .constraints_ = {db_schema_constraint{.name_ = "tenant_users_pk", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"tenant_id", "user_id"}}, db_schema_constraint{.name_ = "tenant_fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"tenant_id", "user_id"}, .referenced_table_ = "tenants", .referenced_columns_ = {"id", "id"}, .on_delete_ = ruvia::db_referential_action::cascade}}};
    schema.create_table(table_value);
    const auto migrations = schema.compile("001_create");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("PRIMARY KEY") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("FOREIGN KEY") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("ON DELETE CASCADE") != std::string_view::npos);
}

TEST(Ruvia, db_schema_partial_gin_index_quotes_identifiers_and_literals) {
    db_schema schema({.driver_ = db_driver::postgresql});
    ruvia::db_query query;
    const auto value = query.value(ruvia::db_value(std::string_view("x' OR 1=1")));
    const auto predicate = query.binary(query.column("state"), ruvia::db_binary_operator::equal, value);
    schema.create_index({.name_ = "events_idx", .table_ = "events", .keys_ = {{.column_ = "payload"}}, .method_ = ruvia::db_index_method::gin, .where_ = predicate});
    const auto migrations = schema.compile("002_index");
    EXPECT_TRUE(migrations[0].sql().find("USING gin") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("x'' OR 1=1") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DROP TABLE") == std::string_view::npos);
}

TEST(Ruvia, db_schema_postgresql_batch_is_atomic_and_unwrapped_isolated) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_schema("one");
    schema.create_schema("two");
    auto migrations = schema.compile("003_batch");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia$ BEGIN") == 0);

    db_schema invalid({.driver_ = db_driver::postgresql});
    invalid.create_schema("one");
    invalid.create_index({.name_ = "idx", .table_ = "t", .keys_ = {{.column_ = "x"}}, .concurrently_ = true});
    EXPECT_TRUE(throws_on([&] { (void)invalid.compile("004_invalid"); }));
}

TEST(Ruvia, db_schema_enum_and_postgresql_only_operations_reject_mariadb) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"open", "closed"};
    schema.create_enum("status", values);
    auto migrations = schema.compile("005_enum");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    db_schema addition({.driver_ = db_driver::postgresql});
    addition.add_enum_value("status", "paused");
    const auto added = addition.compile("006_enum_value");
    EXPECT_TRUE(added[0].atomicity() == ruvia::db_migration_atomicity::unwrapped);

    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_enum("status", values); }));
}

RUVIA_DB_ENTITY(app_events_entity_type, "app.events",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true, .generated_ = true}>,
    ruvia::db_column<"payloads", std::vector<std::string>, ruvia::db_column_options{.data_type_ = db_data_type::jsonb}>)

TEST(Ruvia, db_schema_entity_metadata_preserves_explicit_array_element_types) {
    using entity_type = app_events_entity_type;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>();
    const auto migrations = schema.compile("entity");
    EXPECT_EQ(migrations[0].sql(), "CREATE TABLE \"app\".\"events\" (\"id\" BIGINT GENERATED BY DEFAULT AS IDENTITY NOT NULL, \"payloads\" JSONB[] NOT NULL, CONSTRAINT \"events_pkey\" PRIMARY KEY (\"id\"))");
}

RUVIA_DB_ENTITY(char_records_entity_type, "char_records",
    ruvia::db_column<"code", std::string, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 64}>,
    ruvia::db_column<"codes", std::vector<std::string>, ruvia::db_column_options{.data_type_ = db_data_type::char_value, .length_ = 8}>)

TEST(Ruvia, db_schema_char_entity_and_column_operations_render_with_dialect_limits) {
    using entity_type = char_records_entity_type;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table<entity_type>();
    pg.alter_column_type("char_records", "code", {.data_type_ = db_data_type::char_value, .length_ = 32});
    const auto pg_migrations = pg.compile("char_pg");
    EXPECT_TRUE(pg_migrations[0].sql().find("\"code\" CHAR(64) NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("\"codes\" CHAR(8)[] NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("ALTER TABLE \"char_records\" ALTER COLUMN \"code\" TYPE CHAR(32)") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "char_records", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 255}}}});
    EXPECT_TRUE(maria.compile("char_maria")[0].sql().find("`code` CHAR(255) NOT NULL") != std::string_view::npos);

    db_schema pg_limit({.driver_ = db_driver::postgresql});
    pg_limit.create_table({.name_ = "char_limit", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485760}}}});
    EXPECT_TRUE(pg_limit.compile("char_pg_limit")[0].sql().find("\"code\" CHAR(10485760) NOT NULL") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 10485761}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::mariadb});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 256}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::char_value, .length_ = 8, .precision_ = 2}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_table({.name_ = "invalid_char", .columns_ = {{.name_ = "code", .type_ = {.data_type_ = db_data_type::text, .length_ = 8}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::mariadb});
        invalid.create_table({.name_ = "invalid_decimal", .columns_ = {{.name_ = "amount", .type_ = {.data_type_ = db_data_type::numeric}}}});
    }));
    db_schema timestamps({.driver_ = db_driver::mariadb});
    timestamps.create_table({.name_ = "events", .columns_ = {{.name_ = "at", .type_ = {.data_type_ = db_data_type::timestamp}}}});
    EXPECT_TRUE(timestamps.compile("timestamps")[0].sql().find("`at` DATETIME(6)") != std::string_view::npos);
}

RUVIA_DB_ENTITY(computed_events_entity_type, "computed_events",
    ruvia::db_column<"first_name", ruvia::string>,
    ruvia::db_column<"last_name", ruvia::string>,
    ruvia::db_column<"display_name", ruvia::string,
        ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>)
RUVIA_DB_ENTITY(maria_computed_entity_type, "maria_computed",
    ruvia::db_column<"value", std::int64_t>,
    ruvia::db_column<"double_value", std::int64_t,
        ruvia::db_column_options{.generated_type_ = db_generated_type::virtual_value, .nullable_ = true}>)

TEST(Ruvia, db_schema_entity_computed_columns_render_and_validate) {
    using entity_type = computed_events_entity_type;
    ruvia::db_query expression;
    auto generated = expression.binary(expression.column("first_name"), ruvia::db_binary_operator::concat,
        expression.column("last_name"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<entity_type>({.generated_columns_ = {{.column_ = "display_name", .expression_ = generated}}});
    const auto migrations = schema.compile("computed");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("\"display_name\" TEXT GENERATED ALWAYS AS") != std::string_view::npos);
    EXPECT_TRUE(sql.find(" STORED") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] { schema.create_table<entity_type>(); }));

    using maria_entity_type = maria_computed_entity_type;
    ruvia::db_query maria_expression;
    auto doubled = maria_expression.binary(maria_expression.column("value"), ruvia::db_binary_operator::multiply,
        maria_expression.value(2));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    EXPECT_TRUE(maria.compile("maria_computed")[0].sql().find(" VIRTUAL") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.add_column("maria_computed", {.name_ = "invalid", .type_ = {.data_type_ = db_data_type::big_int}, .generated_type_ = db_generated_type::stored, .as_expression_ = doubled});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema pg({.driver_ = db_driver::postgresql});
        pg.create_table<maria_entity_type>({.generated_columns_ = {{.column_ = "double_value", .expression_ = doubled}}});
    }));
}

RUVIA_DB_ENTITY(invalid_computed_type, "invalid_computed",
    ruvia::db_column<"value", std::int64_t>,
    ruvia::db_column<"total", std::int64_t,
        ruvia::db_column_options{.generated_type_ = db_generated_type::stored}>)

namespace db_schema_column_option_test {

template <typename value_type, auto options>
concept declarable_db_column = requires { typename ruvia::db_column<"value", value_type, options>; };

static_assert(declarable_db_column<std::int64_t, ruvia::db_column_options{.primary_key_ = true, .generated_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true, .create_date_ = true}>);
static_assert(declarable_db_column<std::int32_t, ruvia::db_column_options{.version_ = true}>);
static_assert(declarable_db_column<std::string, ruvia::db_column_options{.delete_date_ = true}>);
// Identity and computed generation exclude each other and read-only columns.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .generated_type_ = db_generated_type::stored}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .read_only_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_type_ = db_generated_type::stored, .read_only_ = true}>);
// Identity needs a non-null integer.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.generated_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.generated_ = true, .nullable_ = true}>);
// A named enum type replaces the scalar type.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.data_type_ = db_data_type::text, .enum_name_ = ruvia::fixed_string{"app.state"}}>);
// version_ is a non-null integer concurrency token.
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.version_ = true}>);
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.nullable_ = true, .version_ = true}>);
// Primary keys cannot be nullable or soft-delete markers.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.primary_key_ = true, .nullable_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.primary_key_ = true, .delete_date_ = true}>);
// One lifecycle role per column, and repository-stamped roles must be writable.
static_assert(!declarable_db_column<std::int64_t, ruvia::db_column_options{.update_date_ = true, .version_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.read_only_ = true, .delete_date_ = true}>);
static_assert(!declarable_db_column<std::string, ruvia::db_column_options{.generated_type_ = db_generated_type::stored, .update_date_ = true}>);

}  // namespace db_schema_column_option_test

TEST(Ruvia, db_schema_computed_columns_reject_invalid_metadata_and_conflicts) {
    using computed_type = invalid_computed_type;
    ruvia::db_query query;
    const auto expression = query.binary(query.column("value"), ruvia::db_binary_operator::add, query.value(1));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "missing", .expression_ = expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"total", expression}, {"total", expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{"value", expression}, {"total", expression}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.generated_columns_ = {{.column_ = "total", .expression_ = {}}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema schema({.driver_ = db_driver::postgresql});
        schema.create_table<computed_type>({.defaults_ = {{.column_ = "total", .value_ = expression}},
            .generated_columns_ = {{.column_ = "total", .expression_ = expression}}});
    }));
}

RUVIA_DB_ENTITY(computed_keys_entity_type, "computed_keys",
    ruvia::db_column<"source", std::int64_t>,
    ruvia::db_column<"computed", std::int64_t,
        ruvia::db_column_options{.primary_key_ = true, .generated_type_ = db_generated_type::stored}>)

TEST(Ruvia, db_schema_mariadb_computed_columns_reject_table_primary_keys) {
    using entity_type = computed_keys_entity_type;
    ruvia::db_query query;
    auto expression = query.column("source");
    db_schema schema({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] {
        schema.create_table<entity_type>({.generated_columns_ = {{"computed", expression}}});
    }));
    for (const bool composite : {false, true}) {
        db_table_definition table_value{.name_ = "computed_keys", .columns_ = {{.name_ = "source", .type_ = {.data_type_ = db_data_type::big_int}}, {.name_ = "computed", .type_ = {.data_type_ = db_data_type::big_int}, .nullable_ = true, .generated_type_ = db_generated_type::stored, .as_expression_ = expression}}, .constraints_ = {{.name_ = "computed_key", .kind_ = ruvia::db_constraint_kind::primary_key, .columns_ = {"computed"}}}};
        if (composite) {
            table_value.constraints_[0].columns_.push_back("source");
        }
        EXPECT_TRUE(throws_on([&] { schema.create_table(table_value); }));
    }
}

TEST(Ruvia, db_schema_entity_relations_emit_owning_foreign_keys_and_one_to_one_unique) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<schema_owner_type>();
    const auto migrations = schema.compile("relations");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"parent_id\",\"parent_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"peer_id\") REFERENCES \"schema_targets\" (\"id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"schema_owners_peer_key\" UNIQUE (\"peer_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("children") == std::string_view::npos);
}

TEST(Ruvia, db_schema_many_to_many_relation_table_has_composite_primary_key_and_two_foreign_keys) {
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_relation_tables<schema_owner_type>();
    const auto migrations = schema.compile("relation_tables");
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE TABLE \"owner_labels\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("PRIMARY KEY (\"owner_id\",\"label_id\",\"label_tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"owner_id\") REFERENCES \"schema_owners\" (\"id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREIGN KEY (\"label_id\",\"label_tenant_id\") REFERENCES \"schema_targets\" (\"id\",\"tenant_id\")") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"label_id\" BIGINT NOT NULL") != std::string_view::npos);
}

TEST(Ruvia, db_schema_qualified_types_and_replace_view_use_valid_ddl_grammar) {
    db_schema schema({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"on", "off"};
    schema.create_enum("app.state", values);
    schema.create_table({.name_ = "app.devices", .columns_ = {{.name_ = "state", .type_ = {.custom_name_ = "app.state"}}}});
    ruvia::db_query query;
    query.select(query.column("state")).from("app.devices");
    schema.create_view("app.device_state", query, true);
    const auto migrations = schema.compile("types");
    EXPECT_TRUE(migrations[0].sql().find("CREATE TYPE \"app\".\"state\" AS ENUM") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"state\" \"app\".\"state\" NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("CREATE OR REPLACE VIEW \"app\".\"device_state\" AS SELECT") != std::string_view::npos);
}

TEST(Ruvia, db_schema_procedure_builds_channel_guard_and_escapes_body_delimiters) {
    ruvia::db_query query;
    ruvia::db_procedure body;
    body.declare_row("channel", "link");
    ruvia::db_query channel;
    channel.from("link").where(channel.binary(channel.column("id"), ruvia::db_binary_operator::equal, channel.column("link_id", "new"))).lock({.mode_ = ruvia::db_row_lock::share});
    const std::array<std::string_view, 1> targets{"channel"};
    body.select_into(channel, targets, true);
    body.begin_if(query.binary(query.column("protocol", "channel"), ruvia::db_binary_operator::not_equal, query.column("protocol", "new")));
    body.raise_exception("channel mismatch $ruvia$ 'quoted'", "23514");
    body.end_if();
    body.assign("new.revision", query.binary(query.column("revision", "old"), ruvia::db_binary_operator::add, query.value(1)));
    body.return_value(query.column("new"));
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_trigger_function("app.bind_channel", body);
    schema.create_trigger({.name_ = "bind_channel", .table_ = "app.device", .function_ = "app.bind_channel", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::update}});
    const auto migrations = schema.compile("guard");
    EXPECT_TRUE(migrations[0].sql().find("\"channel\" \"link\"%ROWTYPE") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("FOR SHARE INTO STRICT \"channel\"") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("AS $ruvia1$") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia2$") == 0);
    EXPECT_TRUE(migrations[0].sql().find("BEFORE INSERT OR UPDATE ON \"app\".\"device\" FOR EACH ROW EXECUTE FUNCTION \"app\".\"bind_channel\"()") != std::string_view::npos);
}

TEST(Ruvia, db_schema_timescale_policies_are_typed_function_calls_and_table_options) {
    ruvia::db_query query;
    auto interval = query.cast(query.value("7 days"), db_data_type::interval);
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_hypertable({.table_ = "telemetry.events", .time_column_ = "ts", .chunk_interval_ = interval, .if_not_exists_ = true});
    schema.set_compression("telemetry.events", {.segment_by_ = {"device_id"}, .order_by_ = {{"ts", ruvia::db_order_direction::desc}}});
    schema.add_compression_policy("telemetry.events", interval, true);
    schema.add_retention_policy("telemetry.events", query.cast(query.value("90 days"), db_data_type::interval));
    const auto migrations = schema.compile("storage");
    EXPECT_TRUE(migrations[0].sql().find("\"create_hypertable\"(E'\"telemetry\".\"events\"', \"by_range\"(E'ts', CAST(E'7 days' AS INTERVAL))") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"timescaledb\".\"compress\" = true") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"compress_after\" => CAST(E'7 days' AS INTERVAL)") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find("\"drop_after\" => CAST(E'90 days' AS INTERVAL)") != std::string_view::npos);
}

TEST(Ruvia, db_schema_single_perform_and_control_flow_have_complete_blocks) {
    ruvia::db_query query;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.perform(query.call("pg_notify", {query.value("channel"), query.value("body $ruvia$")}));
    const auto migrations = schema.compile("notify");
    EXPECT_TRUE(migrations[0].sql().find("DO $ruvia1$ BEGIN PERFORM") == 0);
    ruvia::db_procedure body;
    body.begin_if(query.value(true));
    EXPECT_TRUE(throws_on([&] { (void)body.body(); }));
    body.end_if();
    EXPECT_TRUE(throws_on([&] { body.otherwise(); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.validate_constraint("t", "fk"); }));
    ruvia::db_query removed;
    removed.delete_from("t").returning({removed.column("id")});
    EXPECT_TRUE(throws_on([&] { schema.create_view("v", removed); }));
    ruvia::db_query read;
    read.with("removed", removed).from("removed");
    EXPECT_TRUE(throws_on([&] { schema.create_view("v", read); }));
}

TEST(Ruvia, db_schema_ddl_operations_and_table_options_render_for_both_drivers) {
    ruvia::db_query pg_expressions;
    const auto pg_default = pg_expressions.value("pending");
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_pg", true);
    pg.create_table({.name_ = "cs_pg.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::always},
            {.name_ = "name",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .unique_ = true},
            {.name_ = "state", .type_ = {.data_type_ = db_data_type::text}, .default_value_ = pg_default}},
        .if_not_exists_ = true,
        .unlogged_ = true});
    pg.rename_table("cs_pg.records", "records_renamed");
    pg.add_column("cs_pg.records_renamed",
        {.name_ = "created_at", .type_ = {.data_type_ = db_data_type::timestamp_tz}, .nullable_ = true}, true);
    pg.rename_column("cs_pg.records_renamed", "created_at", "created_on");
    pg.alter_column_type("cs_pg.records_renamed", "state", {.data_type_ = db_data_type::varchar, .length_ = 64},
        pg_expressions.column("state"));
    pg.set_column_nullable("cs_pg.records_renamed", "state", true);
    pg.set_column_nullable("cs_pg.records_renamed", "state", false);
    pg.set_column_default("cs_pg.records_renamed", "state", pg_expressions.value("ready"));
    pg.drop_column_default("cs_pg.records_renamed", "state");
    pg.drop_column("cs_pg.records_renamed", "created_on", ruvia::db_drop_behavior::cascade, true);
    pg.drop_table("cs_pg.records_renamed", ruvia::db_drop_behavior::cascade, true);
    pg.drop_schema("cs_pg", ruvia::db_drop_behavior::cascade, true);
    const auto pg_migrations = pg.compile("cs_pg_ops");
    EXPECT_EQ(pg_migrations.size(), std::size_t{1});
    const auto pg_sql = pg_migrations[0].sql();
    EXPECT_TRUE(pg_sql.find("CREATE SCHEMA IF NOT EXISTS \"cs_pg\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("CREATE UNLOGGED TABLE IF NOT EXISTS \"cs_pg\".\"records\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("GENERATED ALWAYS AS IDENTITY") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("VARCHAR(32) UNIQUE") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("ALTER COLUMN \"state\" TYPE VARCHAR(64) USING \"state\"") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("DROP COLUMN IF EXISTS \"created_on\" CASCADE") != std::string_view::npos);
    EXPECT_TRUE(pg_sql.find("DROP SCHEMA IF EXISTS \"cs_pg\" CASCADE") != std::string_view::npos);

    ruvia::db_query maria_expressions;
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_schema("cs_maria", true);
    maria.create_table({.name_ = "cs_maria.records",
        .columns_ = {{.name_ = "id",
                         .type_ = {.data_type_ = db_data_type::big_int},
                         .identity_ = ruvia::db_identity::by_default},
            {.name_ = "state",
                .type_ = {.data_type_ = db_data_type::varchar, .length_ = 32},
                .nullable_ = true,
                .default_value_ = maria_expressions.value("ready")}},
        .if_not_exists_ = true});
    maria.add_column("cs_maria.records", {.name_ = "note", .type_ = {.data_type_ = db_data_type::text}, .nullable_ = true}, true);
    maria.rename_column("cs_maria.records", "note", "memo");
    maria.set_column_default("cs_maria.records", "memo", maria_expressions.value("n/a"));
    maria.drop_column_default("cs_maria.records", "memo");
    EXPECT_TRUE(throws_on([&] { maria.drop_column("cs_maria.records", "memo", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_column("cs_maria.records", "memo", ruvia::db_drop_behavior::restrict, true);
    maria.add_constraint("cs_maria.records", {.name_ = "records_state_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"state"}});
    EXPECT_TRUE(throws_on([&] { maria.drop_constraint("cs_maria.records", "records_state_unique", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_constraint("cs_maria.records", "records_state_unique", ruvia::db_drop_behavior::restrict, true);
    maria.rename_table("cs_maria.records", "records_renamed");
    EXPECT_TRUE(throws_on([&] { maria.drop_table("cs_maria.records_renamed", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_table("cs_maria.records_renamed", ruvia::db_drop_behavior::restrict, true);
    EXPECT_TRUE(throws_on([&] { maria.drop_schema("cs_maria", ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_schema("cs_maria", ruvia::db_drop_behavior::restrict, true);
    const auto maria_migrations = maria.compile("cs_maria_ops");
    EXPECT_EQ(maria_migrations.size(), std::size_t{12});
    EXPECT_EQ(maria_migrations[0].id(), "cs_maria_ops_1");
    EXPECT_TRUE(maria_migrations[1].sql().find("CREATE TABLE IF NOT EXISTS `cs_maria`.`records`") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[1].sql().find("AUTO_INCREMENT") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[2].sql().find("ADD COLUMN IF NOT EXISTS `note` TEXT") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[9].sql().find("ALTER TABLE `cs_maria`.`records` RENAME TO `cs_maria`.`records_renamed`") != std::string_view::npos);
    EXPECT_TRUE(maria_migrations[10].sql().find("DROP TABLE IF EXISTS `cs_maria`.`records_renamed`") != std::string_view::npos);
    for (const auto& migration : maria_migrations) {
        EXPECT_TRUE(migration.sql().find("CASCADE") == std::string_view::npos);
    }
    EXPECT_TRUE(throws_on([&] {
        maria.alter_column_type("cs_maria.records", "state", {.data_type_ = db_data_type::text});
    }));
    EXPECT_TRUE(throws_on([&] { maria.set_column_nullable("cs_maria.records", "state", true); }));
}

TEST(Ruvia, db_schema_constraints_cover_actions_deferred_not_valid_and_rejections) {
    ruvia::db_query expressions;
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.add_constraint("cs_child",
        {.name_ = "child_fk",
            .kind_ = ruvia::db_constraint_kind::foreign_key,
            .columns_ = {"parent_id"},
            .referenced_table_ = "cs_parent",
            .referenced_columns_ = {"id"},
            .on_delete_ = ruvia::db_referential_action::set_null,
            .on_update_ = ruvia::db_referential_action::set_default,
            .deferrable_ = true,
            .initially_deferred_ = true,
            .not_valid_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_unique", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"parent_id"}, .deferrable_ = true});
    pg.add_constraint("cs_child",
        {.name_ = "child_check",
            .kind_ = ruvia::db_constraint_kind::check,
            .not_valid_ = true,
            .check_ = expressions.binary(expressions.column("amount"), ruvia::db_binary_operator::greater_equal,
                expressions.value(0))});
    pg.validate_constraint("cs_child", "child_fk");
    const auto migrations = pg.compile("cs_constraints");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("ON DELETE SET NULL ON UPDATE SET DEFAULT DEFERRABLE INITIALLY DEFERRED NOT VALID") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"child_unique\" UNIQUE (\"parent_id\") DEFERRABLE") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CONSTRAINT \"child_check\" CHECK ((\"amount\" >= 0)) NOT VALID") != std::string_view::npos);
    EXPECT_TRUE(sql.find("VALIDATE CONSTRAINT \"child_fk\"") != std::string_view::npos);

    db_table_definition invalid_table{.name_ = "cs_invalid",
        .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::big_int}}},
        .constraints_ = {{.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true}}};
    EXPECT_TRUE(throws_on([&] { pg.create_table(invalid_table); }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .initially_deferred_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check, .deferrable_ = true, .check_ = expressions.value(true)});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::unique, .columns_ = {"id"}, .not_valid_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {}});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "bad", .kind_ = ruvia::db_constraint_kind::check});
    }));

    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {"id"}, .deferrable_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.add_constraint("cs_child", {.name_ = "check", .kind_ = ruvia::db_constraint_kind::check, .not_valid_ = true, .check_ = expressions.value(true)});
    }));
}

TEST(Ruvia, db_schema_constructor_table_flags_and_compile_boundaries_are_checked) {
    EXPECT_TRUE(throws_on([&] { db_schema invalid({}); }));
    db_schema empty({.driver_ = db_driver::postgresql});
    EXPECT_TRUE(empty.compile("cs_empty").empty());
    EXPECT_TRUE(throws_on([&] { (void)empty.compile(""); }));

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_table({.name_ = "cs_temp_pg", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    EXPECT_TRUE(pg.compile("cs_temp_pg")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_table({.name_ = "cs_temp_maria", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true});
    EXPECT_TRUE(maria.compile("cs_temp_maria")[0].sql().find("CREATE TEMPORARY TABLE") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_invalid_flags", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .temporary_ = true, .unlogged_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_unlogged", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}, .unlogged_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_table({.name_ = "cs_always", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}, .identity_ = ruvia::db_identity::always}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.create_table({.name_ = "cs_duplicate", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}, {.name_ = "id", .type_ = {.data_type_ = db_data_type::integer}}}});
    }));
}

TEST(Ruvia, db_schema_index_variants_and_dialect_rejections) {
    ruvia::db_query expressions;
    const auto predicate = expressions.binary(expressions.column("active"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc, .nulls_ = ruvia::db_nulls_order::last, .operator_class_ = "text_ops"}, {.expression_ = expressions.column("lower_name")}}, .unique_ = true, .if_not_exists_ = true, .method_ = ruvia::db_index_method::btree, .include_ = {"id"}, .where_ = predicate});
    const auto pg_migrations = pg.compile("cs_index");
    EXPECT_EQ(pg_migrations.size(), std::size_t{1});
    EXPECT_TRUE(pg_migrations[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS \"cs_idx\" ON \"cs_records\" USING btree") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("\"name\" \"text_ops\" DESC NULLS LAST, (\"lower_name\")") != std::string_view::npos);
    EXPECT_TRUE(pg_migrations[0].sql().find("INCLUDE (\"id\") WHERE (\"active\" = TRUE)") != std::string_view::npos);

    for (const auto method : {ruvia::db_index_method::hash, ruvia::db_index_method::gin, ruvia::db_index_method::gist,
             ruvia::db_index_method::sp_gist, ruvia::db_index_method::brin}) {
        db_schema method_schema({.driver_ = db_driver::postgresql});
        method_schema.create_index({.name_ = "cs_method", .table_ = "cs_records", .keys_ = {{.column_ = "name"}}, .method_ = method});
        EXPECT_TRUE(method_schema.compile("cs_method")[0].sql().find("USING ") != std::string_view::npos);
    }
    db_schema concurrent({.driver_ = db_driver::postgresql});
    concurrent.create_index({.name_ = "cs_concurrent", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    const auto concurrent_migrations = concurrent.compile("cs_concurrent");
    EXPECT_EQ(concurrent_migrations[0].atomicity(), ruvia::db_migration_atomicity::unwrapped);
    db_schema drop({.driver_ = db_driver::postgresql});
    drop.drop_index("cs_records.cs_idx", true, true);
    EXPECT_TRUE(drop.compile("cs_drop_index")[0].sql().find("DROP INDEX CONCURRENTLY IF EXISTS \"cs_records\".\"cs_idx\"") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id", .expression_ = expressions.column("id")}}});
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}, {.column_ = ""}}});
    }));
    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_index({.name_ = "cs_maria_idx", .table_ = "cs_records", .keys_ = {{.column_ = "name", .order_ = ruvia::db_order_direction::desc}}, .unique_ = true, .if_not_exists_ = true});
    EXPECT_TRUE(maria.compile("cs_maria_idx")[0].sql().find("CREATE UNIQUE INDEX IF NOT EXISTS `cs_maria_idx` USING btree ON `cs_records`") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .method_ = ruvia::db_index_method::hash});
    }));
    EXPECT_TRUE(throws_on([&] {
        maria.create_index({.name_ = "bad", .table_ = "t", .keys_ = {{.column_ = "id"}}, .include_ = {"extra"}});
    }));
    EXPECT_TRUE(throws_on([&] { maria.drop_index("idx"); }));
}

TEST(Ruvia, db_schema_views_enums_extensions_and_commands_cover_dialect_boundaries) {
    ruvia::db_query query;
    query.select(query.column("id")).from("cs_records");
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array<std::string_view, 2> values{"new", "done"};
    pg.create_enum("cs_status", values);
    pg.drop_enum("cs_status", ruvia::db_drop_behavior::cascade, true);
    pg.create_extension("hstore", true);
    pg.create_view("cs_view", query, true);
    pg.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true);
    pg.create_view("cs_mat", query, false, true);
    pg.drop_view("cs_mat", true, ruvia::db_drop_behavior::restrict, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    pg.execute(command);
    const auto migrations = pg.compile("cs_objects");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE TYPE \"cs_status\" AS ENUM (E'new',E'done')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP TYPE IF EXISTS \"cs_status\" CASCADE") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE EXTENSION IF NOT EXISTS \"hstore\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE OR REPLACE VIEW \"cs_view\" AS SELECT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP MATERIALIZED VIEW IF EXISTS \"cs_mat\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("UPDATE \"cs_records\" SET \"state\" = E'done'") != std::string_view::npos);
    db_schema enum_addition({.driver_ = db_driver::postgresql});
    enum_addition.add_enum_value("cs_status", "archived", true);
    const auto enum_migrations = enum_addition.compile("cs_enum_addition");
    EXPECT_EQ(enum_migrations.size(), std::size_t{1});
    EXPECT_TRUE(enum_migrations[0].sql().find("ALTER TYPE \"cs_status\" ADD VALUE IF NOT EXISTS E'archived'") != std::string_view::npos);

    db_schema maria({.driver_ = db_driver::mariadb});
    maria.create_view("cs_view", query);
    EXPECT_TRUE(throws_on([&] { maria.drop_view("cs_view", false, ruvia::db_drop_behavior::cascade, true); }));
    maria.drop_view("cs_view", false, ruvia::db_drop_behavior::restrict, true);
    maria.execute(command);
    EXPECT_TRUE(maria.compile("cs_objects").size() == std::size_t{3});
    EXPECT_TRUE(throws_on([&] { maria.create_enum("status", values); }));
    EXPECT_TRUE(throws_on([&] { maria.add_enum_value("status", "x"); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_enum("status"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_extension("hstore"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_view("mat", query, false, true); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_view("mat", true); }));
    EXPECT_TRUE(throws_on([&] { pg.create_view("bad", command); }));
    EXPECT_TRUE(throws_on([&] { pg.create_view("bad", query, true, true); }));
    EXPECT_TRUE(throws_on([&] { pg.create_enum("empty", {}); }));
    ruvia::db_query returned;
    returned.delete_from("cs_records").returning({returned.column("id")});
    EXPECT_TRUE(throws_on([&] { pg.execute(returned); }));
}

TEST(Ruvia, db_schema_trigger_variants_cover_events_timing_when_arguments_and_rejections) {
    ruvia::db_query expressions;
    const auto when = expressions.binary(expressions.column("enabled"), ruvia::db_binary_operator::equal, expressions.value(true));
    db_procedure procedure;
    procedure.return_value();
    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_trigger_function("cs_trigger_fn", procedure, true);
    pg.create_trigger({.name_ = "cs_before_insert", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::insert}});
    pg.create_trigger({.name_ = "cs_after_update", .table_ = "cs_records", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::after, .events_ = {ruvia::db_trigger_event::update}, .update_columns_ = {"state", "enabled"}, .when_ = when, .arguments_ = {"arg 'one'", "two"}});
    pg.create_trigger({.name_ = "cs_statement", .table_ = "cs_records", .function_ = "cs_trigger_fn", .events_ = {ruvia::db_trigger_event::delete_value, ruvia::db_trigger_event::truncate}, .for_each_row_ = false});
    pg.create_trigger({.name_ = "cs_instead", .table_ = "cs_records_view", .function_ = "cs_trigger_fn", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}});
    pg.drop_trigger("cs_records", "cs_after_update", true);
    pg.drop_trigger_function("cs_trigger_fn", ruvia::db_drop_behavior::cascade, true);
    const auto migrations = pg.compile("cs_triggers");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("CREATE OR REPLACE FUNCTION \"cs_trigger_fn\"() RETURNS trigger LANGUAGE plpgsql") != std::string_view::npos);
    EXPECT_TRUE(sql.find("AFTER UPDATE OF \"state\", \"enabled\" ON \"cs_records\" FOR EACH ROW WHEN ((\"enabled\" = TRUE)) EXECUTE FUNCTION \"cs_trigger_fn\"(E'arg ''one''', E'two')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("AFTER UPDATE OF") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DELETE OR TRUNCATE ON \"cs_records\" FOR EACH STATEMENT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("INSTEAD OF INSERT ON \"cs_records_view\" FOR EACH ROW") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP TRIGGER IF EXISTS \"cs_after_update\" ON \"cs_records\"") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP FUNCTION IF EXISTS \"cs_trigger_fn\"() CASCADE") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f"}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::insert}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}, .update_columns_ = {"x"}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::truncate}}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .for_each_row_ = false}); }));
    EXPECT_TRUE(throws_on([&] { pg.create_trigger({.name_ = "bad", .table_ = "t", .function_ = "f", .timing_ = ruvia::db_trigger_timing::instead_of, .events_ = {ruvia::db_trigger_event::insert}, .when_ = when}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { maria.create_trigger_function("f", procedure); }));
    EXPECT_TRUE(throws_on([&] { maria.create_trigger({.name_ = "f", .table_ = "t", .function_ = "f", .events_ = {ruvia::db_trigger_event::insert}}); }));
    EXPECT_TRUE(throws_on([&] { maria.drop_trigger("t", "f"); }));
    EXPECT_TRUE(throws_on([&] { maria.perform(expressions.value(true)); }));
    EXPECT_TRUE(throws_on([&] { maria.run(procedure); }));
}

TEST(Ruvia, db_schema_timescale_options_cover_direct_settings_and_rejections) {
    ruvia::db_query expressions;
    const auto interval = expressions.cast(expressions.value("1 day"), db_data_type::interval);
    db_schema pg({.driver_ = db_driver::postgresql});
    const std::array table_options{db_table_option{"autovacuum_enabled", true}, db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"toast_tuple_target", "128"}};
    pg.set_table_options("cs_events", table_options);
    pg.set_database_option("cs_db", "timescaledb.max_background_workers", "8");
    pg.set_chunk_time_interval("cs_events", interval);
    pg.set_compression("cs_events", {.enabled_ = false});
    pg.remove_compression_policy("cs_events", true);
    pg.remove_retention_policy("cs_events", true);
    const auto migrations = pg.compile("cs_timescale_options");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("ALTER TABLE \"cs_events\" SET (\"autovacuum_enabled\" = true, \"fillfactor\" = 80, \"toast_tuple_target\" = E'128')") != std::string_view::npos);
    EXPECT_TRUE(sql.find("ALTER DATABASE \"cs_db\" SET \"timescaledb\".\"max_background_workers\" TO E'8'") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"set_chunk_time_interval\"(E'\"cs_events\"', CAST(E'1 day' AS INTERVAL))") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"timescaledb\".\"compress\" = false") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"remove_compression_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"remove_retention_policy\"(E'\"cs_events\"', \"if_exists\" => TRUE)") != std::string_view::npos);
    EXPECT_TRUE(throws_on([&] { pg.set_table_options("cs_events", std::span<const db_table_option>()); }));
    const std::array duplicate_options{db_table_option{"fillfactor", std::int64_t{80}}, db_table_option{"fillfactor", std::int64_t{90}}};
    EXPECT_TRUE(throws_on([&] {
        pg.set_table_options("cs_events", duplicate_options);
    }));
    EXPECT_TRUE(throws_on([&] { pg.set_compression("cs_events", {.enabled_ = false, .segment_by_ = {"device_id"}}); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    const std::array maria_options{db_table_option{"x", true}};
    EXPECT_TRUE(throws_on([&] { maria.set_table_options("t", maria_options); }));
    EXPECT_TRUE(throws_on([&] { maria.set_database_option("d", "x", "y"); }));
    EXPECT_TRUE(throws_on([&] { maria.create_hypertable({.table_ = "t", .time_column_ = "ts"}); }));
    EXPECT_TRUE(throws_on([&] { maria.set_chunk_time_interval("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.set_compression("t", {}); }));
    EXPECT_TRUE(throws_on([&] { maria.add_compression_policy("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.remove_compression_policy("t"); }));
    EXPECT_TRUE(throws_on([&] { maria.add_retention_policy("t", interval); }));
    EXPECT_TRUE(throws_on([&] { maria.remove_retention_policy("t"); }));
}

TEST(Ruvia, db_procedure_public_helpers_render_and_validate) {
    ruvia::db_query expressions;
    ruvia::db_procedure procedure;
    procedure.declare_variable({.name_ = "counter", .type_ = {.data_type_ = db_data_type::integer}, .default_value_ = expressions.value(0)});
    procedure.declare_row("row_value", "cs_records");
    procedure.declare_record("record_value");
    procedure.assign("counter", expressions.binary(expressions.column("counter"), ruvia::db_binary_operator::add, expressions.value(1)));
    ruvia::db_query select;
    select.select(select.column("id")).from("cs_records");
    const std::array<std::string_view, 1> targets{"row_value"};
    procedure.select_into(select, targets, false);
    procedure.select_into(select, targets, true);
    ruvia::db_query command;
    command.update("cs_records").set("state", command.value("done"));
    procedure.execute(command);
    procedure.perform(expressions.call("pg_notify", {expressions.value("cs"), expressions.value("body")}));
    procedure.begin_if(expressions.column("enabled"));
    procedure.else_if(expressions.column("fallback"));
    procedure.otherwise();
    procedure.end_if();
    procedure.begin_block();
    procedure.catch_sql_state("23505");
    procedure.end_block();
    procedure.return_value();
    procedure.raise_exception("failed", "P0001");
    const auto body = procedure.body();
    EXPECT_TRUE(body.find("DECLARE\n") == 0);
    EXPECT_TRUE(body.find("\"counter\" INTEGER := 0;") != std::string_view::npos);
    EXPECT_TRUE(body.find("\"row_value\" \"cs_records\"%ROWTYPE;") != std::string_view::npos);
    EXPECT_TRUE(body.find("SELECT \"id\" FROM \"cs_records\" INTO \"row_value\";") != std::string_view::npos);
    EXPECT_TRUE(body.find("INTO STRICT \"row_value\";") != std::string_view::npos);
    EXPECT_TRUE(body.find("ELSIF \"fallback\" THEN") != std::string_view::npos);
    EXPECT_TRUE(body.find("EXCEPTION\nWHEN SQLSTATE E'23505'") != std::string_view::npos);
    EXPECT_TRUE(body.find("RETURN;\nRAISE EXCEPTION USING MESSAGE = E'failed'") != std::string_view::npos);

    db_schema pg({.driver_ = db_driver::postgresql});
    pg.create_schema("cs_nested");
    ruvia::db_procedure applied;
    applied.apply(pg);
    EXPECT_TRUE(applied.body().find("CREATE SCHEMA \"cs_nested\";") != std::string_view::npos);
    db_schema non_transactional({.driver_ = db_driver::postgresql});
    non_transactional.create_index({.name_ = "cs_idx", .table_ = "cs_records", .keys_ = {{.column_ = "id"}}, .concurrently_ = true});
    EXPECT_TRUE(throws_on([&] { applied.apply(non_transactional); }));
    db_schema maria({.driver_ = db_driver::mariadb});
    EXPECT_TRUE(throws_on([&] { applied.apply(maria); }));

    EXPECT_TRUE(throws_on([&] { procedure.declare_record("counter"); }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.execute(select);
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(command, targets);
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.select_into(select, {});
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.else_if(expressions.value(true));
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.otherwise();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_if();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.catch_sql_state("23505");
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.begin_block();
        invalid.catch_sql_state("bad");
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.end_block();
    }));
    EXPECT_TRUE(throws_on([&] {
        ruvia::db_procedure invalid;
        invalid.raise_exception("bad", "00000");
    }));
}

RUVIA_DB_ENTITY(devices_read_only_entity, "devices",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"protocol_address", std::string, ruvia::db_column_options{.read_only_ = true}>)

TEST(Ruvia, db_schema_trigger_transition_tables_and_read_only_columns_render_and_validate) {
    using read_only_entity = devices_read_only_entity;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<read_only_entity>();
    schema.create_trigger({
        .name_ = "devices_live_update",
        .table_ = "devices",
        .function_ = "publish_query_change",
        .timing_ = ruvia::db_trigger_timing::after,
        .events_ = {ruvia::db_trigger_event::update},
        .for_each_row_ = false,
        .old_table_ = "old_rows",
        .new_table_ = "new_rows",
        .arguments_ = {"device"},
    });
    const auto migrations = schema.compile("transition_trigger");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    EXPECT_TRUE(migrations[0].sql().find("\"protocol_address\" TEXT NOT NULL") != std::string_view::npos);
    EXPECT_TRUE(migrations[0].sql().find(
                    "CREATE TRIGGER \"devices_live_update\" AFTER UPDATE ON \"devices\" "
                    "REFERENCING OLD TABLE AS \"old_rows\" NEW TABLE AS \"new_rows\" "
                    "FOR EACH STATEMENT EXECUTE FUNCTION \"publish_query_change\"(E'device')") != std::string_view::npos);

    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_before",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::before,
            .events_ = {ruvia::db_trigger_event::insert},
            .for_each_row_ = false,
            .new_table_ = "new_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_multi_event",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::insert, ruvia::db_trigger_event::update},
            .for_each_row_ = false,
            .new_table_ = "new_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_old_insert",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::insert},
            .for_each_row_ = false,
            .old_table_ = "old_rows",
        });
    }));
    EXPECT_TRUE(throws_on([&] {
        db_schema invalid({.driver_ = db_driver::postgresql});
        invalid.create_trigger({
            .name_ = "bad_same_name",
            .table_ = "devices",
            .function_ = "fn",
            .timing_ = ruvia::db_trigger_timing::after,
            .events_ = {ruvia::db_trigger_event::update},
            .for_each_row_ = false,
            .old_table_ = "rows",
            .new_table_ = "rows",
        });
    }));
}

// Enum values outside the declared enumerators (reachable only through a cast)
// are rejected instead of rendering a default clause.
TEST(Ruvia, db_schema_rejects_unknown_enum_values_instead_of_defaulting) {
    db_schema pg({.driver_ = db_driver::postgresql});
    EXPECT_TRUE(throws_on([&] { pg.drop_table("cs_records", static_cast<ruvia::db_drop_behavior>(7)); }));
    EXPECT_TRUE(throws_on([&] {
        pg.add_constraint("cs_child", {.name_ = "child_fk", .kind_ = ruvia::db_constraint_kind::foreign_key, .columns_ = {"parent_id"}, .referenced_table_ = "cs_parent", .referenced_columns_ = {"id"}, .on_delete_ = static_cast<ruvia::db_referential_action>(9)});
    }));
    EXPECT_TRUE(throws_on([&] {
        pg.create_trigger({.name_ = "cs_trigger", .table_ = "cs_records", .function_ = "fn", .timing_ = static_cast<ruvia::db_trigger_timing>(9), .events_ = {ruvia::db_trigger_event::insert}});
    }));
}

RUVIA_DB_ENTITY(versioned_articles_entity, "versioned_articles",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"revision", std::int64_t, ruvia::db_column_options{.version_ = true}>,
    ruvia::db_column<"created_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .create_date_ = true}>,
    ruvia::db_column<"updated_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .update_date_ = true}>,
    ruvia::db_column<"deleted_at", std::string, ruvia::db_column_options{.data_type_ = db_data_type::timestamp_tz, .delete_date_ = true}>)

TEST(Ruvia, db_schema_and_procedure_functions_loops_dynamic_sql_and_table_locks) {
    using versioned_entity = versioned_articles_entity;
    db_schema schema({.driver_ = db_driver::postgresql});
    schema.create_table<versioned_entity>();
    schema.lock_table("versioned_articles", ruvia::db_table_lock_mode::access_exclusive, true);

    ruvia::db_query sql_fn_body;
    sql_fn_body.select(sql_fn_body.value(true));
    schema.create_function(
        {
            .name_ = "is_article_ready",
            .parameters_ = {{.name_ = "article_id", .type_ = {.data_type_ = db_data_type::big_int}}},
            .return_type_ = {.data_type_ = db_data_type::boolean},
            .volatility_ = ruvia::db_function_volatility::stable,
            .strict_ = true,
            .replace_ = true,
        },
        sql_fn_body);

    ruvia::db_query loop_query;
    loop_query.select(loop_query.column("id")).from("versioned_articles");
    ruvia::db_query expressions;
    db_procedure proc;
    proc.declare_record("rec");
    proc.declare_variable({.name_ = "op", .type_ = {.data_type_ = db_data_type::text}});
    proc.lock_table("versioned_articles", ruvia::db_table_lock_mode::share);
    proc.begin_for_query("rec", loop_query);
    proc.begin_foreach("op", expressions.array({expressions.value("INSERT"), expressions.value("UPDATE")}));
    proc.execute_dynamic(
        expressions.call("format", {expressions.value("SELECT %L, %L"), expressions.column("id", "rec"), expressions.column("op")}));
    proc.end_loop();
    proc.end_loop();
    schema.run(proc);
    const std::array<ruvia::db_type_definition, 1> drop_params{{{.data_type_ = db_data_type::big_int}}};
    schema.drop_function("is_article_ready", drop_params, ruvia::db_drop_behavior::cascade, true);

    const auto migrations = schema.compile("functions_and_loops");
    EXPECT_EQ(migrations.size(), std::size_t{1});
    const auto sql = migrations[0].sql();
    EXPECT_TRUE(sql.find("\"revision\" BIGINT NOT NULL DEFAULT 1") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"created_at\" TIMESTAMPTZ NOT NULL DEFAULT \"now\"()") != std::string_view::npos);
    EXPECT_TRUE(sql.find("\"deleted_at\" TIMESTAMPTZ") != std::string_view::npos);
    EXPECT_TRUE(sql.find("LOCK TABLE \"versioned_articles\" IN ACCESS EXCLUSIVE MODE NOWAIT") != std::string_view::npos);
    EXPECT_TRUE(sql.find("CREATE OR REPLACE FUNCTION \"is_article_ready\"(\"article_id\" BIGINT) RETURNS BOOLEAN LANGUAGE sql STABLE STRICT AS") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOR \"rec\" IN SELECT \"id\" FROM \"versioned_articles\" LOOP") != std::string_view::npos);
    EXPECT_TRUE(sql.find("FOREACH \"op\" IN ARRAY ARRAY[E'INSERT', E'UPDATE'] LOOP") != std::string_view::npos);
    EXPECT_TRUE(sql.find("EXECUTE \"format\"(E'SELECT %L, %L', \"rec\".\"id\", \"op\");") != std::string_view::npos);
    EXPECT_TRUE(sql.find("DROP FUNCTION IF EXISTS \"is_article_ready\"(BIGINT) CASCADE") != std::string_view::npos);
}

namespace {

RUVIA_DB_ENTITY(entity, "users",
    ruvia::db_column<ruvia::fixed_string{"id"}, int>,
    ruvia::db_column<ruvia::fixed_string{"name"}, std::string,
        ruvia::db_column_options{.nullable_ = true}>)

RUVIA_DB_ENTITY(owned_type, "owned", ruvia::db_column<"text", ruvia::string>,
    ruvia::db_column<"values", std::vector<std::optional<std::string>>>)

RUVIA_DB_ENTITY(string_array_entity, "string_arrays",
    ruvia::db_column<"values", std::vector<std::string>>)

RUVIA_DB_ENTITY(owned_container_entity, "owned_containers",
    ruvia::db_column<"text", std::string>,
    ruvia::db_column<"values", std::vector<std::string>>)

RUVIA_DB_ENTITY(numeric_input_entity, "numeric_input",
    ruvia::db_column<"signed_value", std::int32_t, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"unsigned_value", std::uint32_t, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"real_value", float, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"values", std::vector<std::int32_t>>,
    ruvia::db_column<"optional_values", std::vector<std::optional<std::int32_t>>>,
    ruvia::db_column<"real_values", std::vector<float>>)

TEST(Ruvia, db_entity_tracks_unset_null_and_value_states) {
    entity entity;
    EXPECT_TRUE(!entity.is_set<"id">());
    entity.set<"id">(7);
    EXPECT_TRUE(entity.is_set<"id">());
    EXPECT_EQ(entity.get<"id">(), 7);
    const auto& const_entity = entity;
    EXPECT_EQ(const_entity.get<"id">(), 7);
    entity.set_null<"name">();
    EXPECT_TRUE(entity.is_null<"name">());
    entity.reset<"id">();
    EXPECT_TRUE(!entity.is_set<"id">());
    EXPECT_EQ(entity::table_name(), std::string_view("users"));
    EXPECT_EQ(entity::column_index<"name">(), std::size_t{1});
}

TEST(Ruvia, db_entity_numeric_overflow_preserves_value_null_and_unset) {
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    const auto below_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)()) - 1;
    const auto above_unsigned = static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1;
    for (int state = 0; state != 3; ++state) {
        numeric_input_entity value;
        if (state == 1) {
            value.set_null<"signed_value">().set_null<"unsigned_value">().set_null<"real_value">();
        } else if (state == 2) {
            value.set<"signed_value">(7).set<"unsigned_value">(9).set<"real_value">(1.5);
        }
        const auto check_rejected = [&](auto assign) {
            bool rejected = false;
            try {
                assign();
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        };
        check_rejected([&] { value.set<"signed_value">(above_signed); });
        check_rejected([&] { value.set<"signed_value">(below_signed); });
        check_rejected([&] { value.set<"signed_value">((std::numeric_limits<std::uint64_t>::max)()); });
        check_rejected([&] { value.set<"signed_value">(ruvia::int64{above_signed}); });
        check_rejected([&] { value.set<"unsigned_value">(-1); });
        check_rejected([&] { value.set<"unsigned_value">(above_unsigned); });
        check_rejected([&] { value.set<"real_value">((std::numeric_limits<double>::max)()); });
        check_rejected([&] { value.set<"real_value">(-(std::numeric_limits<double>::max)()); });
        check_rejected([&] { value.set<"real_value">(ruvia::double_value{(std::numeric_limits<double>::max)()}); });
        EXPECT_EQ(value.is_set<"signed_value">(), state != 0);
        EXPECT_EQ(value.is_set<"unsigned_value">(), state != 0);
        EXPECT_EQ(value.is_set<"real_value">(), state != 0);
        EXPECT_EQ(value.is_null<"signed_value">(), state == 1);
        EXPECT_EQ(value.is_null<"unsigned_value">(), state == 1);
        EXPECT_EQ(value.is_null<"real_value">(), state == 1);
        if (state == 2) {
            EXPECT_EQ(value.get<"signed_value">(), 7);
            EXPECT_EQ(value.get<"unsigned_value">(), 9U);
            EXPECT_EQ(value.get<"real_value">(), 1.5F);
        }
    }
}

TEST(Ruvia, db_entity_numeric_arrays_reject_late_overflow_without_partial_replacement) {
    numeric_input_entity value;
    value.set<"values">(std::vector<std::int32_t>({11, 12}));
    value.set<"optional_values">(std::vector<std::optional<std::int32_t>>({11, std::nullopt, 12}));
    value.set<"real_values">(std::vector<float>({1.5F, 2.5F}));
    const auto above_signed = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) + 1;
    const auto check_rejected = [&](auto assign) {
        bool rejected = false;
        try {
            assign();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    };
    check_rejected([&] { value.set<"values">(std::vector<std::int64_t>({42, above_signed})); });
    check_rejected([&] {
        value.set<"optional_values">(std::vector<std::optional<std::int64_t>>({42, std::nullopt, above_signed}));
    });
    check_rejected([&] { value.set<"real_values">(std::vector<double>({0.5, (std::numeric_limits<double>::max)()})); });
    EXPECT_EQ(value.get<"values">().size(), std::size_t{2});
    EXPECT_EQ(value.get<"values">()[0], 11);
    EXPECT_EQ(value.get<"values">()[1], 12);
    EXPECT_EQ(value.get<"optional_values">().size(), std::size_t{3});
    EXPECT_EQ(*value.get<"optional_values">()[0], 11);
    EXPECT_TRUE(!value.get<"optional_values">()[1]);
    EXPECT_EQ(*value.get<"optional_values">()[2], 12);
    EXPECT_EQ(value.get<"real_values">().size(), std::size_t{2});
    EXPECT_EQ(value.get<"real_values">()[0], 1.5F);
    EXPECT_EQ(value.get<"real_values">()[1], 2.5F);
}

TEST(Ruvia, db_entity_numeric_set_accepts_representable_boundaries_and_scalar_wrappers) {
    numeric_input_entity value;
    const auto minimum = static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)());
    const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    value.set<"signed_value">(ruvia::int64{minimum});
    EXPECT_EQ(value.get<"signed_value">(), minimum);
    value.set<"signed_value">(maximum);
    EXPECT_EQ(value.get<"signed_value">(), (std::numeric_limits<std::int32_t>::max)());
    value.set<"unsigned_value">(static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()));
    EXPECT_EQ(value.get<"unsigned_value">(), (std::numeric_limits<std::uint32_t>::max)());
    value.set<"values">(std::vector<std::int64_t>({minimum, static_cast<std::int64_t>(maximum)}));
    EXPECT_EQ(value.get<"values">()[0], minimum);
    EXPECT_EQ(value.get<"values">()[1], (std::numeric_limits<std::int32_t>::max)());
    value.set<"real_value">(ruvia::double_value{static_cast<double>((std::numeric_limits<float>::max)())});
    EXPECT_EQ(value.get<"real_value">(), (std::numeric_limits<float>::max)());
    value.set<"real_value">(std::numeric_limits<double>::infinity());
    EXPECT_EQ(value.get<"real_value">(), std::numeric_limits<float>::infinity());
    value.set<"real_values">(std::vector<std::int64_t>({-7, 9}));
    EXPECT_EQ(value.get<"real_values">()[0], -7.0F);
    EXPECT_EQ(value.get<"real_values">()[1], 9.0F);
}

TEST(Ruvia, db_entity_set_owns_nested_values_and_supports_reset) {
    owned_type entity;
    {
        ruvia::string text(std::string(200, 'x'));
        entity.set<"text">(std::move(text));
        std::vector<std::optional<std::string>> values;
        values.emplace_back(std::string(200, 'y'));
        values.emplace_back(std::nullopt);
        entity.set<"values">(std::move(values));
    }
    EXPECT_EQ(std::string_view(entity.get<"text">()), std::string(200, 'x'));
    EXPECT_EQ(std::string_view(*entity.get<"values">()[0]), std::string(200, 'y'));
    entity.reset<"text">();
    entity.reset<"values">();
    EXPECT_FALSE(entity.is_set<"text">());
    EXPECT_FALSE(entity.is_set<"values">());
}

}  // namespace

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_DB_ENTITY(numeric_decode_entity, "numeric_decode",
    ruvia::db_column<"id", std::int64_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"ratio", double>,
    ruvia::db_column<"ratios", std::vector<double>>,
    ruvia::db_column<"counts", std::vector<std::int64_t>>)

TEST(Ruvia, db_field_and_array_decoding_reject_nonfinite_floats_and_prioritize_format_errors) {
    using namespace ruvia;
    std::vector<test::backend_startup_peer::postgres_reply> replies({
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("nan")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("nan(123)")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("inf")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("-infinity")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("999999999999999999999abc")})},
        {.column_ = std::string("val"), .rows_ = std::vector<std::string>({std::string("999999999999999999999")})},
        {.column_ = std::string("ratios"), .type_ = 1022, .rows_ = std::vector<std::string>({std::string("{nan}")})},
        {.column_ = std::string("ratios"), .type_ = 1022, .rows_ = std::vector<std::string>({std::string("{inf}")})},
        {.column_ = std::string("counts"), .type_ = 1016, .rows_ = std::vector<std::string>({std::string("{999999999999999999999abc}")})},
        {.column_ = std::string("counts"), .type_ = 1016, .rows_ = std::vector<std::string>({std::string("{999999999999999999999}")})},
    });
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        for (int i = 0; i < 4; ++i) {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            EXPECT_EQ(rows.size(), std::size_t{1});
            bool invalid_double = false;
            bool invalid_float = false;
            try {
                (void)rows.front()["val"].as<double>();
            } catch (const db_conversion_error& error) {
                invalid_double = error.code() == db_conversion_error::code_type::invalid_format;
            }
            try {
                (void)rows.front()["val"].as<float>();
            } catch (const db_conversion_error& error) {
                invalid_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_double);
            EXPECT_TRUE(invalid_float);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool invalid_format = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                invalid_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_format);
        }
        {
            const auto rows = co_await handle.query("SELECT 1 AS val");
            bool out_of_range = false;
            try {
                (void)rows.front()["val"].as<std::int64_t>();
            } catch (const db_conversion_error& error) {
                out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            EXPECT_TRUE(out_of_range);
        }

        auto repo = handle.repository<numeric_decode_entity>();
        for (int i = 0; i < 2; ++i) {
            bool invalid_array_float = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"ratios"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_float = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_array_float);
        }
        {
            bool invalid_array_int_format = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                invalid_array_int_format = error.code() == db_conversion_error::code_type::invalid_format;
            }
            EXPECT_TRUE(invalid_array_int_format);
        }
        {
            bool array_int_out_of_range = false;
            try {
                auto qb = repo.query_builder("n");
                qb.select({"counts"});
                (void)co_await qb.get_many();
            } catch (const db_conversion_error& error) {
                array_int_out_of_range = error.code() == db_conversion_error::code_type::out_of_range;
            }
            EXPECT_TRUE(array_int_out_of_range);
        }
    });
}
#endif

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace {
using namespace ruvia;
RUVIA_DB_ENTITY(mapped_user, "users", db_column<"id", int>, db_column<"name", std::string>)

TEST(Ruvia, db_public_mapping_owns_values_and_rejects_missing_or_ambiguous_columns) {
    using peer_type = test::backend_startup_peer;
    const std::string name(128, 'n');
    std::vector<peer_type::postgres_reply> replies;
    replies.push_back({.columns_ = {"name", "id", "ignored"}, .values_ = {{name, "7", "extra"}}});
    replies.push_back({.column_ = "values", .rows_ = {R"({"comma,quote\"slash\\",NULL,"NULL"})"}});
    replies.push_back({.rows_ = {""}, .null_rows_ = {true}});
    replies.push_back({.column_ = "id", .rows_ = {"8"}});
    replies.push_back({.columns_ = {"id", "id", "name"}, .values_ = {{"1", "2", "ambiguous"}}});
    peer_type peer(peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        std::optional<mapped_user> owned;
        {
            const auto rows = co_await handle.query("SELECT name, id, ignored FROM users");
            owned.emplace(decode_db_row<mapped_user>(rows.front()));
            const auto batch = decode_db_rows<mapped_user>(rows);
            EXPECT_EQ(batch[0].get<"id">(), 7);
            EXPECT_EQ(batch[0].get<"name">(), name);
        }
        EXPECT_EQ(owned->get<"id">(), 7);
        EXPECT_EQ(owned->get<"name">(), name);
        using array_type = std::vector<std::optional<std::string>>;
        array_type values;
        {
            const auto rows = co_await handle.query("SELECT values FROM arrays");
            values = decode_db_field<array_type>(rows.front()["values"]);
        }
        EXPECT_EQ(values.size(), std::size_t{3});
        if (values.size() == 3) {
            EXPECT_EQ(values[0], "comma,quote\"slash\\");
            EXPECT_FALSE(values[1]);
            EXPECT_EQ(values[2], "NULL");
        }
        {
            const auto rows = co_await handle.query("SELECT NULL AS v");
            EXPECT_FALSE(decode_db_field<std::optional<std::string>>(rows.front()["v"]));
            bool rejected = false;
            try {
                (void)decode_db_field<int>(rows.front()["v"]);
            } catch (const db_conversion_error& error) {
                rejected = error.code() == db_conversion_error::code_type::unexpected_null;
            }
            EXPECT_TRUE(rejected);
        }
        for (const auto expected : {db_conversion_error::code_type::missing_column,
                 db_conversion_error::code_type::invalid_format}) {
            const auto rows = co_await handle.query("SELECT * FROM invalid_mapping");
            bool rejected = false;
            try {
                (void)decode_db_row<mapped_user>(rows.front());
            } catch (const db_conversion_error& error) {
                rejected = error.code() == expected;
            }
            EXPECT_TRUE(rejected);
        }
    });
}
}  // namespace
#endif

TEST(Ruvia, db_transaction_options_support_cold_cancellation_and_reject_invalid_values) {
    using namespace ruvia;
    const std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
        db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
        db_driver::mariadb,
#endif
    };
    for (const auto driver : drivers) {
        auto& context_value = test::new_test_io_context();
        auto attachment = attach_event_loop(context_value, {.queue_capacity_ = 8});
        test::with_connected_db_client(attachment, db_config{.driver_ = driver}, [&](db_client& client) -> task<void> {
            auto handle = client.db();
            stop_source cancellation;
            cancellation.request_stop();
            auto cancelled = handle.with_options({.stop_token_ = cancellation.token()});
            auto exercise = [&]() -> task<void> {
                for (int iteration = 0; iteration < 8; ++iteration) {
                    {
                        const auto cold = handle.begin_transaction({.isolation_ = db_transaction_isolation::serializable,
                            .access_mode_ = db_transaction_access_mode::read_only});
                    }
                    bool observed_cancellation = false;
                    bool observed_invalid = false;
                    db_transaction_options options{.isolation_ = db_transaction_isolation::repeatable_read,
                        .access_mode_ = db_transaction_access_mode::read_only};
                    auto operation = cancelled.begin_transaction(options);
                    options.isolation_ = static_cast<db_transaction_isolation>(255);
                    try {
                        auto transaction = co_await std::move(operation);
                        EXPECT_TRUE(false);
                    } catch (const db_error& error) {
                        observed_cancellation = error.code() == db_error::code_type::cancelled;
                    }
                    auto invalid = handle.begin_transaction(options);
                    options = {};
                    try {
                        auto transaction = co_await std::move(invalid);
                        EXPECT_TRUE(false);
                    } catch (const std::invalid_argument&) {
                        observed_invalid = true;
                    }
                    EXPECT_TRUE(observed_cancellation);
                    EXPECT_TRUE(observed_invalid);
                }
            };
            co_await exercise();
        });
    }
}

#ifdef RUVIA_ENABLE_POSTGRESQL
namespace db_transaction_operation_test {

RUVIA_DB_ENTITY(text_entity, "records", ruvia::db_column<"v", std::string>)

struct queued_transaction_operations final {
    explicit queued_transaction_operations(ruvia::db_transaction& transaction)
        : commit_(transaction.commit()),
          rollback_(transaction.rollback()),
          query_(transaction.query("SELECT v FROM records")),
          execute_(transaction.execute("UPDATE records SET v = 'raw'")),
          compiled_query_(transaction.repository<text_entity>().query_builder().get_raw_many()),
          compiled_execute_(transaction.repository<text_entity>()
                  .delete_builder()
                  .where(text_entity::column<"v">().is_not_null())
                  .execute()),
          mapped_(transaction.repository<text_entity>().find()),
          page_(transaction.repository<text_entity>().find_and_count()) {}

    ruvia::task<void> commit_;
    ruvia::task<void> rollback_;
    ruvia::task<ruvia::db_rows> query_;
    ruvia::task<ruvia::db_exec_result> execute_;
    ruvia::task<ruvia::db_rows> compiled_query_;
    ruvia::task<ruvia::db_exec_result> compiled_execute_;
    ruvia::task<ruvia::entity_rows<text_entity>> mapped_;
    ruvia::task<std::pair<ruvia::entity_rows<text_entity>, std::uint64_t>> page_;
};

struct queued_stream_operations final {
    explicit queued_stream_operations(ruvia::db_stream_result& stream)
        : close_(stream.close()),
          read_(stream.read()) {}

    ruvia::task<void> close_;
    ruvia::task<std::optional<ruvia::db_row>> read_;
};

TEST(Ruvia, db_transaction_can_prepare_cold_operations_while_a_statement_is_running) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("first")}), .status_ = 'T', .deferred_ = true},
            {.rows_ = std::vector<std::string>({std::string("raw")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("UPDATE 2"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("compiled")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("DELETE 4"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("mapped")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("counted")}), .status_ = 'T'},
            {.column_ = std::string("count"), .rows_ = std::vector<std::string>({std::string("1")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto transaction = co_await handle.begin_transaction();
        bool first_completed = false;
        task_scope running(connected.worker());
        const auto first = [&]() -> task<void> {
            const auto rows = co_await transaction.query("SELECT v FROM records");
            first_completed = rows.size() == 1 && rows.front()["v"].as<std::string_view>() == "first";
        };
        running.spawn(first());
        std::unique_ptr<queued_transaction_operations> queued;
        bool concurrent_created = false;
        bool concurrent_rejected = false;
        std::exception_ptr failure;
        try {
            queued = std::make_unique<queued_transaction_operations>(transaction);
        } catch (const std::logic_error&) {
            queued.reset();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            auto concurrent = transaction.query("SELECT v FROM records");
            concurrent_created = true;
            try {
                (void)co_await std::move(concurrent);
            } catch (const std::logic_error&) {
                concurrent_rejected = true;
            }
        } catch (const std::logic_error&) {
            concurrent_created = false;
        } catch (...) {
            failure = std::current_exception();
        }
        peer.release_replies();
        co_await running.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_TRUE(first_completed);
        EXPECT_TRUE(transaction.active());
        EXPECT_TRUE(queued != nullptr);
        EXPECT_TRUE(concurrent_created && concurrent_rejected);
        if (!queued) {
            co_return;
        }
        const auto raw = co_await std::move(queued->query_);
        EXPECT_EQ(raw.size(), std::size_t{1});
        if (!raw.empty()) {
            EXPECT_TRUE(raw.front()["v"].as<std::string_view>() == "raw");
        }
        const auto executed = co_await std::move(queued->execute_);
        EXPECT_EQ(executed.affected_rows(), std::uint64_t{2});
        const auto compiled = co_await std::move(queued->compiled_query_);
        EXPECT_EQ(compiled.size(), std::size_t{1});
        if (!compiled.empty()) {
            EXPECT_TRUE(compiled.front()["v"].as<std::string_view>() == "compiled");
        }
        const auto changed = co_await std::move(queued->compiled_execute_);
        EXPECT_EQ(changed.affected_rows(), std::uint64_t{4});
        const auto mapped = co_await std::move(queued->mapped_);
        EXPECT_EQ(mapped.size(), std::size_t{1});
        if (!mapped.empty()) {
            EXPECT_EQ(mapped[0].get<"v">(), std::string_view("mapped"));
        }
        const auto page = co_await std::move(queued->page_);
        EXPECT_EQ(page.first.size(), std::size_t{1});
        EXPECT_EQ(page.second, std::uint64_t{1});
        if (!page.first.empty()) {
            EXPECT_EQ(page.first[0].get<"v">(), std::string_view("counted"));
        }
        co_await std::move(queued->commit_);
        EXPECT_TRUE(!transaction.active());
    });
}

TEST(Ruvia, db_stream_cold_reads_and_close_wait_for_the_started_read_to_finish) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = {"prefetched", "first", "second"},
            .command_ = "SELECT 3",
            .deferred_ = true,
            .first_row_before_hold_ = true}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto handle = connected.db();
        auto stream = co_await handle.query_stream("SELECT v FROM records");
        const auto prefetched = co_await stream.read();
        EXPECT_TRUE(prefetched && (*prefetched)["v"].as<std::string_view>() == "prefetched");
        bool first_completed = false;
        task_scope reading(connected.worker());
        const auto first = [&]() -> task<void> {
            const auto row = co_await stream.read();
            first_completed = row && (*row)["v"].as<std::string_view>() == "first";
        };
        reading.spawn(first());
        std::exception_ptr failure;
        std::unique_ptr<queued_stream_operations> queued;
        bool rejected = false;
        try {
            queued = std::make_unique<queued_stream_operations>(stream);
            auto concurrent = stream.read();
            try {
                (void)co_await std::move(concurrent);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        peer.release_replies();
        co_await reading.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        EXPECT_TRUE(rejected);
        EXPECT_TRUE(first_completed);
        const auto row = co_await std::move(queued->read_);
        EXPECT_TRUE(row.has_value());
        if (row) {
            EXPECT_TRUE((*row)["v"].as<std::string_view>() == "second");
        }
        EXPECT_TRUE(!(co_await stream.read()));
        EXPECT_TRUE(!stream.active());
    });
}

TEST(Ruvia, db_stream_close_after_exhaustion_is_idempotent) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = std::vector<std::string>({std::string("only")}), .command_ = std::string("SELECT 1")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto stream = co_await connected.db().query_stream("SELECT v FROM records");
        while (co_await stream.read()) {
        }
        EXPECT_TRUE(!stream.active());
        co_await stream.close();
        EXPECT_TRUE(!stream.active());
    });
}

// A read's own operation refines the stream's options. An already-stopped
// token is refused before the connection is touched, so the stream stays
// usable; once the read waits, its timeout or stop request closes the
// connection and ends the stream.
TEST(Ruvia, db_stream_read_takes_its_own_operation_options) {
    using namespace ruvia;
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel ? "cancelled" : "timed out");
        test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
            test::backend_startup_peer::after_startup::idle,
            std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.rows_ = {"first", "withheld"},
                .command_ = "SELECT 2",
                .deferred_ = true,
                .first_row_before_hold_ = true}}));
        auto& context = test::new_test_io_context();
        auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
        db_client client(attachment.loop(), peer.config());
        test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
            auto stream = co_await connected.db().query_stream("SELECT v FROM records");
            const auto failure_of = [](task<std::optional<db_row>> read) -> task<std::optional<db_error::code_type>> {
                try {
                    (void)co_await std::move(read);
                } catch (const db_error& error) {
                    co_return error.code();
                }
                co_return std::nullopt;
            };
            stop_source stopped;
            stopped.request_stop();
            EXPECT_TRUE(co_await failure_of(stream.read({.stop_token_ = stopped.token()})) == db_error::code_type::cancelled);
            EXPECT_TRUE(stream.active());
            EXPECT_THROW((void)stream.read({.timeout_ = std::chrono::milliseconds(0)}), std::invalid_argument);
            const auto first = co_await stream.read();
            EXPECT_TRUE(first && (*first)["v"].as<std::string_view>() == "first");
            stop_source stop;
            task_scope stopper(connected.worker());
            const auto stop_later = [&]() -> task<void> {
                (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(30));
                stop.request_stop();
            };
            operation_options operation{.timeout_ = std::chrono::milliseconds(30)};
            if (cancel) {
                operation = {.stop_token_ = stop.token()};
                stopper.spawn(stop_later());
            }
            const auto code = co_await failure_of(stream.read(std::move(operation)));
            co_await stopper.join();
            EXPECT_TRUE(code == (cancel ? db_error::code_type::cancelled : db_error::code_type::timeout));
            EXPECT_TRUE(!stream.active());
        });
    }
}

TEST(Ruvia, db_transaction_savepoint_commands_quote_identifiers_and_keep_transaction_active) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("SAVEPOINT"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("ROLLBACK"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("RELEASE"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.db().begin_transaction();
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)transaction.savepoint(""); }));
        co_await transaction.savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.rollback_to_savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.release_savepoint("stage_1");
        EXPECT_TRUE(transaction.active());
        co_await transaction.commit();
        EXPECT_TRUE(!transaction.active());
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{5});
    if (statements.size() == 5) {
        EXPECT_EQ(statements[1].sql_, "SAVEPOINT \"stage_1\"");
        EXPECT_EQ(statements[2].sql_, "ROLLBACK TO SAVEPOINT \"stage_1\"");
        EXPECT_EQ(statements[3].sql_, "RELEASE SAVEPOINT \"stage_1\"");
    }
}

TEST(Ruvia, db_transaction_advisory_locks_notify_and_cursors_execute_in_order) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::idle,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("t")}), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("DECLARE CURSOR"), .status_ = 'T'},
            {.column_ = std::string("v"), .rows_ = std::vector<std::string>({std::string("chunk-1"), std::string("chunk-2")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("CLOSE CURSOR"), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    db_client client(attachment.loop(), peer.config());
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        auto transaction = co_await connected.db().begin_transaction();
        co_await transaction.advisory_lock(7001);
        co_await transaction.advisory_lock(17, 1);
        co_await transaction.advisory_lock("resource-key", 99);
        EXPECT_TRUE(co_await transaction.try_advisory_lock(7001));
        co_await transaction.notify("iot_outbox_pending", "ready");

        co_await transaction.declare_cursor("chunk_cursor", "SELECT \"v\" FROM \"records\"");
        const auto fetched = co_await transaction.fetch_cursor("chunk_cursor", 8);
        EXPECT_EQ(fetched.size(), std::size_t{2});
        co_await transaction.close_cursor("chunk_cursor");
        co_await transaction.commit();
    });
    const auto statements = peer.postgres_parses();
    EXPECT_EQ(statements.size(), std::size_t{10});
    if (statements.size() == 10) {
        EXPECT_EQ(statements[1].sql_, "SELECT \"pg_advisory_xact_lock\"($1::bigint)");
        EXPECT_EQ(statements[2].sql_, "SELECT \"pg_advisory_xact_lock\"($1::integer, $2::integer)");
        EXPECT_EQ(statements[3].sql_, "SELECT \"pg_advisory_xact_lock\"(\"hashtextextended\"($1::text, $2::bigint))");
        EXPECT_EQ(statements[4].sql_, "SELECT \"pg_try_advisory_xact_lock\"($1::bigint)");
        EXPECT_EQ(statements[5].sql_, "SELECT \"pg_notify\"($1::text, $2::text)");
        EXPECT_EQ(statements[6].sql_, "DECLARE \"chunk_cursor\" NO SCROLL CURSOR FOR SELECT \"v\" FROM \"records\"");
        EXPECT_EQ(statements[7].sql_, "FETCH FORWARD 8 FROM \"chunk_cursor\"");
        EXPECT_EQ(statements[8].sql_, "CLOSE \"chunk_cursor\"");
    }
}

TEST(Ruvia, db_transaction_scalar_decode_failure_releases_slot_for_next_transaction) {
    using namespace ruvia;
    test::backend_startup_peer peer(test::backend_startup_peer::protocol::postgresql, false,
        test::backend_startup_peer::after_startup::reaccept,
        std::vector<ruvia::test::backend_startup_peer::postgres_reply>({{.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("invalid-bool")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("BEGIN"), .status_ = 'T'},
            {.rows_ = std::vector<std::string>({std::string("t")}), .status_ = 'T'},
            {.column_ = std::string(""), .command_ = std::string("COMMIT")}}));
    auto& context = test::new_test_io_context();
    auto attachment = attach_event_loop(context, {.queue_capacity_ = 8});
    auto config = peer.config();
    config.pool_size_per_worker_ = 1;
    db_client client(attachment.loop(), config);
    test::drive_backend_client(attachment, client, [&](db_client& connected) -> task<void> {
        {
            auto transaction = co_await connected.db().begin_transaction();
            bool failed = false;
            try {
                (void)co_await transaction.try_advisory_lock(7001);
            } catch (const db_conversion_error&) {
                failed = true;
            }
            EXPECT_TRUE(failed);
            EXPECT_TRUE(!transaction.active());
        }
        auto next = co_await connected.db().begin_transaction();
        EXPECT_TRUE(co_await next.try_advisory_lock(7001));
        co_await next.commit();
    });
}
}  // namespace db_transaction_operation_test
#endif

namespace db_transaction_timeout_test {

using peer_type = ruvia::test::backend_startup_peer;

constexpr std::array drivers{
#ifdef RUVIA_ENABLE_POSTGRESQL
    ruvia::db_driver::postgresql,
#endif
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_driver::mariadb,
#endif
};

// What the peer answers, in order, on its one connection.
enum class step {
    begin,
    // One row inside the transaction.
    row,
    // One row inside the transaction, held until release_replies().
    held_row,
    commit,
    // Two rows outside a transaction, for a stream.
    stream_rows,
};

[[nodiscard]] std::unique_ptr<peer_type> make_peer(ruvia::db_driver driver, std::initializer_list<step> steps) {
    if (driver == ruvia::db_driver::postgresql) {
        std::vector<peer_type::postgres_reply> replies{};
        for (const auto value : steps) {
            switch (value) {
                case step::begin:
                    replies.push_back({.column_ = "", .command_ = "BEGIN", .status_ = 'T'});
                    break;
                case step::row:
                    replies.push_back({.rows_ = {"kept"}, .status_ = 'T'});
                    break;
                case step::held_row:
                    replies.push_back({.rows_ = {"late"}, .status_ = 'T', .deferred_ = true});
                    break;
                case step::commit:
                    replies.push_back({.column_ = "", .command_ = "COMMIT"});
                    break;
                case step::stream_rows:
                    replies.push_back({.rows_ = {"first", "second"}, .command_ = "SELECT 2"});
                    break;
            }
        }
        return std::make_unique<peer_type>(
            peer_type::protocol::postgresql, false, peer_type::after_startup::idle, std::move(replies));
    }
    // MariaDB status 3: autocommit with a transaction open.
    std::vector<peer_type::maria_reply> replies{};
    for (const auto value : steps) {
        switch (value) {
            case step::begin:
                replies.push_back({.column_ = "", .status_ = 3});
                break;
            case step::row:
                replies.push_back({.rows_ = {"kept"}, .status_ = 3});
                break;
            case step::held_row:
                replies.push_back({.rows_ = {"late"}, .status_ = 3, .deferred_ = true});
                break;
            case step::commit:
                replies.push_back({.column_ = "", .status_ = 2});
                break;
            case step::stream_rows:
                replies.push_back({.rows_ = {"first", "second"}});
                break;
        }
    }
    return std::make_unique<peer_type>(std::move(replies), peer_type::after_startup::idle);
}

[[nodiscard]] bool sent_statement(peer_type& peer, ruvia::db_driver driver, std::string_view sql) {
    if (driver == ruvia::db_driver::postgresql) {
        const auto parses = peer.postgres_parses();
        return std::ranges::any_of(parses, [&](const auto& parse) { return parse.sql_ == sql; });
    }
    const auto commands = peer.maria_commands();
    return std::ranges::any_of(
        commands, [&](const std::string& command) { return command.find(sql) != std::string::npos; });
}

// Waits for the peer's sessions to end on a blocking-pool thread, so the
// worker keeps running the timers and closes under test meanwhile. The peer
// outlives the wait: drive_client() joins the pool before returning.
ruvia::task<void> await_disconnects(const ruvia::worker_ref& worker, peer_type& peer) {
    auto waited = co_await ruvia::run_blocking(worker, [target = &peer] { target->wait_for_disconnects(); });
    waited.value();
}

template <typename callback_type>
void drive_client(peer_type& peer, ruvia::db_driver driver,
    std::optional<ruvia::timeout_duration> transaction_timeout, callback_type&& callback) {
    auto config = peer.config();
    config.driver_ = driver;
    config.transaction_timeout_ = transaction_timeout;
    ruvia::blocking_pool waits({.thread_count_ = 1});
    auto& context_value = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(context_value, {.queue_capacity_ = 8, .blocking_pool_ = &waits});
    ruvia::db_client client(attachment.loop(), config);
    try {
        ruvia::test::drive_backend_client(attachment, client, std::forward<callback_type>(callback));
    } catch (...) {
        waits.join();
        throw;
    }
    waits.join();
}

TEST(Ruvia, db_config_transaction_timeout_defaults_to_one_minute_and_rejects_non_positive_values) {
    using namespace ruvia;
    EXPECT_TRUE(db_config{}.transaction_timeout_->value() == std::chrono::milliseconds(60000));
    event_loop_pool pool({.loop_count_ = 1});
    for (const auto driver : drivers) {
        for (const auto value : {std::chrono::milliseconds(0), std::chrono::milliseconds(-1)}) {
            const db_config config{.driver_ = driver, .transaction_timeout_ = value};
            bool client_rejected = false;
            bool migrator_rejected = false;
            try {
                db_client client(pool.loop(0), config);
            } catch (const std::invalid_argument&) {
                client_rejected = true;
            }
            try {
                db_migrator migrator(pool.loop(0), config);
            } catch (const std::invalid_argument&) {
                migrator_rejected = true;
            }
            EXPECT_TRUE(client_rejected);
            EXPECT_TRUE(migrator_rejected);
        }
    }
    pool.join();
}

// The holder keeps the transaction while it awaits something else, such as a
// remote call. The lease timer closes the session at the deadline, so the
// server rolls back and releases the locks without waiting for the holder;
// commit then reports the timeout and sends nothing.
TEST(Ruvia, db_transaction_held_idle_past_its_deadline_closes_the_session_and_fails_commit) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin});
        std::optional<db_error::code_type> code;
        bool ended = false;
        drive_client(*peer, driver, std::chrono::milliseconds(100), [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
            // Throws unless the session already ended while the transaction sat idle.
            co_await await_disconnects(connected.worker(), *peer);
            try {
                co_await transaction.commit();
            } catch (const db_error& error) {
                code = error.code();
            }
            ended = !transaction.active();
        });
        EXPECT_TRUE(code == db_error::code_type::timeout);
        EXPECT_TRUE(ended);
        EXPECT_TRUE(!sent_statement(*peer, driver, "COMMIT"));
    }
}

// Half the transaction's time passes idle; the statement then waits on a reply
// that never comes. query_timeout_ (30 s) alone would let it wait on: it gets
// only what is left of the transaction, fails with timeout, ends the
// transaction and closes the connection instead of returning it to the pool.
TEST(Ruvia, db_transaction_statement_is_bounded_by_the_remaining_transaction_time) {
    using namespace ruvia;
    constexpr auto lease = std::chrono::milliseconds(600);
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin, step::held_row});
        std::optional<db_error::code_type> code;
        std::chrono::steady_clock::duration statement_time{};
        bool ended = false;
        drive_client(*peer, driver, lease, [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), lease / 2);
            const auto started = std::chrono::steady_clock::now();
            try {
                (void)co_await transaction.query("SELECT v FROM records");
            } catch (const db_error& error) {
                code = error.code();
            }
            statement_time = std::chrono::steady_clock::now() - started;
            ended = !transaction.active();
            peer->release_replies();
            // Throws unless the timed-out connection was closed.
            co_await await_disconnects(connected.worker(), *peer);
        });
        EXPECT_TRUE(code == db_error::code_type::timeout);
        EXPECT_TRUE(statement_time < lease);
        EXPECT_TRUE(ended);
    }
}

// Without a transaction deadline, idle time between operations ends nothing:
// the same idle period that expires a 100 ms transaction above leaves this one
// committing on its original session.
TEST(Ruvia, db_transaction_without_a_deadline_keeps_its_session_while_idle) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        auto peer = make_peer(driver, {step::begin, step::row, step::commit});
        std::string value{};
        bool committed = false;
        drive_client(*peer, driver, std::nullopt, [&](db_client& connected) -> task<void> {
            auto transaction = co_await connected.db().begin_transaction();
            (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
            const auto rows = co_await transaction.query("SELECT v FROM records");
            if (!rows.empty()) {
                value = rows.front()["v"].as<std::string_view>().value_or("");
            }
            co_await transaction.commit();
            committed = !transaction.active();
        });
        EXPECT_TRUE(value == "kept");
        EXPECT_TRUE(committed);
        EXPECT_TRUE(sent_statement(*peer, driver, "COMMIT"));
        EXPECT_EQ(peer->startups(), std::size_t{1});
    }
}

// A stream held between reads past its deadline loses its session the same
// way; either terminal operation reports timeout, even with unread rows buffered.
TEST(Ruvia, db_stream_held_idle_past_its_deadline_closes_the_session_and_fails_read_or_close) {
    using namespace ruvia;
    for (const auto driver : drivers) {
        for (const bool close : {false, true}) {
            auto peer = make_peer(driver, {step::stream_rows});
            std::string first{};
            std::optional<db_error::code_type> code;
            bool ended = false;
            drive_client(*peer, driver, std::chrono::milliseconds(100), [&](db_client& connected) -> task<void> {
                auto stream = co_await connected.db().query_stream("SELECT v FROM records");
                if (const auto row = co_await stream.read(); row) {
                    first = (*row)["v"].as<std::string_view>().value_or("");
                }
                (void)co_await sleep_for(connected.worker(), std::chrono::milliseconds(300));
                // Throws unless the session already ended while the stream sat idle.
                co_await await_disconnects(connected.worker(), *peer);
                try {
                    if (close) {
                        co_await stream.close();
                    } else {
                        (void)co_await stream.read();
                    }
                } catch (const db_error& error) {
                    code = error.code();
                }
                ended = !stream.active();
                co_await stream.close();
            });
            EXPECT_TRUE(first == "first");
            EXPECT_TRUE(code == db_error::code_type::timeout);
            EXPECT_TRUE(ended);
        }
    }
}

}  // namespace db_transaction_timeout_test

#if defined(RUVIA_ENABLE_POSTGRESQL)

namespace {

ruvia::task<void> connect_until_stopped(ruvia::db_client& client, bool& cancelled) {
    try {
        co_await client.connect();
    } catch (const ruvia::db_error& error) {
        if (error.code() != ruvia::db_error::code_type::closing) {
            throw;
        }
        cancelled = true;
    }
}

ruvia::task<bool> can_create_operation(ruvia::db_client& client) {
    try {
        // A cold operation exercises the public connected-client contract
        // without requiring the startup peer to implement query execution.
        auto operation = client.db().query("SELECT 1");
        (void)operation;
        co_return true;
    } catch (const std::logic_error&) {
        co_return false;
    }
}

}  // namespace

TEST(Ruvia, db_client_rejects_duplicate_connect_without_interrupting_startup) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    auto duplicate = loop.start(client.connect());
    duplicate.wait();
    peer.authenticate();
    first.wait();
    auto ready = loop.start(can_create_operation(client));
    ready.wait();
    auto closing = loop.start(client.shutdown());
    closing.wait();
    pool.join();
    std::move(first).get();
    std::move(closing).get();
    bool rejected = false;
    try {
        std::move(duplicate).get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(std::move(ready).get());
}

TEST(Ruvia, db_client_rejects_duplicate_connect_without_closing_connected_client) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    peer.authenticate();
    first.wait();
    auto duplicate = loop.start(client.connect());
    duplicate.wait();
    auto ready = loop.start(can_create_operation(client));
    ready.wait();
    auto closing = loop.start(client.shutdown());
    closing.wait();
    pool.join();
    std::move(first).get();
    std::move(closing).get();
    bool rejected = false;
    try {
        std::move(duplicate).get();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(std::move(ready).get());
}

TEST(Ruvia, db_client_fresh_close_and_loop_stop_share_worker_completion) {
    for (int iteration = 0; iteration < 64; ++iteration) {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::db_client client(pool.loop(0), {.driver_ = ruvia::db_driver::postgresql});
        pool.start();
        std::barrier rendezvous(2);
        std::thread closer([&] {
            rendezvous.arrive_and_wait();
            client.close();
        });
        rendezvous.arrive_and_wait();
        pool.stop();
        closer.join();
        pool.join();
        EXPECT_TRUE(!client.worker().accepting());
    }
}

TEST(Ruvia, db_client_cold_connect_can_be_discarded_before_pool_start) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    {
        ruvia::db_client client(pool.loop(0), {.driver_ = ruvia::db_driver::postgresql});
        auto cold = client.connect();
        (void)cold;
    }
    pool.join();
    EXPECT_TRUE(!pool.loop(0).worker().accepting());
}

TEST(Ruvia, db_client_event_loop_stop_awaits_retirement_of_pending_authentication) {
    for (const bool use_attachment_run : {false, true}) {
        ruvia::test::backend_startup_peer peer;
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io);
        ruvia::db_client client(attachment.loop(), peer.config());
        bool cancelled = false;
        auto root = attachment.loop().start(connect_until_stopped(client, cancelled));
        std::thread driver([&] {
            if (use_attachment_run) {
                attachment.run();
            } else {
                io.run();
            }
        });
        std::exception_ptr peer_failure;
        try {
            peer.wait_for_startup();
        } catch (...) {
            peer_failure = std::current_exception();
        }
        attachment.stop();
        driver.join();
        std::move(root).get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(cancelled);
        EXPECT_TRUE(!client.worker().accepting());
    }
}

TEST(Ruvia, db_client_shutdown_waiters_join_pending_authentication) {
    ruvia::test::backend_startup_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto loop = pool.loop(0);
    ruvia::db_client client(loop, peer.config());
    pool.start();
    auto first = loop.start(client.connect());
    peer.wait_for_startup();
    auto closing = loop.start(client.shutdown());
    auto also_closing = loop.start(client.shutdown());
    first.wait();
    closing.wait();
    also_closing.wait();
    auto ready = loop.start(can_create_operation(client));
    ready.wait();
    auto closed_again = loop.start(client.shutdown());
    closed_again.wait();
    pool.join();
    bool cancelled = false;
    try {
        std::move(first).get();
    } catch (const ruvia::db_error& error) {
        if (error.code() != ruvia::db_error::code_type::closing) {
            throw;
        }
        cancelled = true;
    }
    std::move(closing).get();
    std::move(also_closing).get();
    std::move(closed_again).get();
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(!std::move(ready).get());
}

#endif
