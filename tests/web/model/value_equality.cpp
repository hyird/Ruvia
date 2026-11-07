#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"

namespace {

RUVIA_MODEL(comparable_model,
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(comparable_tree,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<comparable_model>),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<comparable_tree>));

}  // namespace

TEST(Ruvia, model_equality_compares_state_and_value_but_not_presence) {
    comparable_model missing;
    comparable_model expected_value;
    expected_value.set<"number">(7);
    EXPECT_TRUE(missing != expected_value);

    auto parsed_value = ruvia::from_json<comparable_model>(R"({"number":7})");
    EXPECT_TRUE(parsed_value.is_present<"number">());
    EXPECT_TRUE(parsed_value == expected_value);

    expected_value.set<"number">(8);
    EXPECT_TRUE(parsed_value != expected_value);
    expected_value.set<"number">(nullptr);
    EXPECT_TRUE(parsed_value != expected_value);
}

TEST(Ruvia, model_equality_distinguishes_missing_and_null) {
    comparable_model missing;
    comparable_model null_value;
    null_value.set<"number">(nullptr);
    EXPECT_TRUE(missing != null_value);

    missing.set<"number">(nullptr);
    EXPECT_TRUE(missing == null_value);
}

TEST(Ruvia, model_equality_ignores_resources_for_normalized_values) {
    ruvia::test::counting_memory_resource left_resource;
    ruvia::test::counting_memory_resource right_resource;
    comparable_model left({.resource_ = &left_resource});
    comparable_model right({.resource_ = &right_resource});
    left.set<"text">(std::string_view("same value"));
    right.set<"text">(std::string_view("same value"));
    EXPECT_TRUE(left == right);

    right.set<"text">(std::string_view("different"));
    EXPECT_TRUE(left != right);
}

TEST(Ruvia, model_equality_compares_nested_arrays_and_recursive_boxed_values) {
    ruvia::test::counting_memory_resource first_resource;
    ruvia::test::counting_memory_resource second_resource;
    constexpr auto input = R"({"value":1,"items":[{"text":"a"},{"text":"b"}],"children":[{"value":2,"children":[{"value":3}]}]})";
    {
        auto first = ruvia::from_json<comparable_tree>(input, {.resource_ = &first_resource});
        auto second = ruvia::from_json<comparable_tree>(input, {.resource_ = &second_resource});
        EXPECT_TRUE(first == second);
        second.ensure<"items">()[0].set<"text">("b");
        EXPECT_TRUE(first != second);
        second.ensure<"items">()[0].set<"text">("a");
        EXPECT_TRUE(first == second);
        second.ensure<"children">().front().ensure<"children">().front().set<"value">(4);
        EXPECT_TRUE(first != second);
    }
    EXPECT_EQ(first_resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(second_resource.live_allocations(), std::size_t{0});
}
