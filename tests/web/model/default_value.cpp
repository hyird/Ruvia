#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {

std::size_t evaluations{0};

int evaluated_default() {
    ++evaluations;
    return 7;
}

int failing_default_value() {
    ++evaluations;
    throw std::runtime_error("default evaluation failed");
}

std::string owned_default_value() {
    ++evaluations;
    return std::string(128, 'd');
}

ruvia::json_value dynamic_null_default() {
    ++evaluations;
    return ruvia::json_value::parse("null");
}

RUVIA_MODEL(optional_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_DEFAULT(evaluated_default()), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(nullable_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_NULLABLE, RUVIA_DEFAULT(evaluated_default())));
RUVIA_MODEL(null_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_NULLABLE, RUVIA_DEFAULT(nullptr)));
RUVIA_MODEL(nullable_dynamic_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE, RUVIA_DEFAULT(dynamic_null_default())));
RUVIA_MODEL(nonnullable_dynamic_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_DEFAULT(dynamic_null_default())));
RUVIA_MODEL(failing_default_model,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_DEFAULT(failing_default_value())));
RUVIA_MODEL(owned_default_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(owned_default_value())));

}  // namespace

TEST(Ruvia, model_default_expression_is_not_evaluated_for_supplied_or_invalid_input) {
    evaluations = 0;
    optional_default constructed;
    EXPECT_TRUE(!constructed.get<"value">());
    EXPECT_EQ(evaluations, std::size_t{0});
    const auto valid_json = ruvia::from_json<optional_default>(R"({"value":9})");
    EXPECT_EQ(evaluations, std::size_t{0});
    EXPECT_EQ(valid_json.get<"value">()->value_, 9);
    for (auto input : {R"({"value":null})", R"({"value":"wrong"})",
             R"({"value":8,"value":9})", R"({"value":)"}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<optional_default>(input); }));
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    auto null = ruvia::from_json<nullable_default>(R"({"value":null})");
    EXPECT_TRUE(null.is_null<"value">());
    EXPECT_EQ(evaluations, std::size_t{0});
    const auto valid_form = ruvia::from_form<optional_default>("value=9");
    EXPECT_EQ(valid_form.get<"value">()->value_, 9);
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_form<optional_default>("value=wrong"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_form<optional_default>("value=8&value=9"); }));
    EXPECT_EQ(evaluations, std::size_t{0});
}

TEST(Ruvia, model_default_expression_runs_once_for_absence_not_for_moves) {
    evaluations = 0;
    auto parsed_value = ruvia::from_json<optional_default>("{}");
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_TRUE(!parsed_value.is_present<"value">());
    EXPECT_EQ(parsed_value.get<"value">()->value_, std::int32_t{7});
    optional_default moved(std::move(parsed_value));
    optional_default assigned;
    assigned = std::move(moved);
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(assigned.get<"value">()->value_, std::int32_t{7});
    assigned.reset<"value">();
    EXPECT_TRUE(!assigned.get<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    const auto form = ruvia::from_form<optional_default>("");
    EXPECT_TRUE(!form.is_present<"value">() && form.get<"value">()->value_ == 7);
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_default_expression_exceptions_unwind_partial_owned_fields) {
    evaluations = 0;
    ruvia::test::counting_memory_resource memory;
    const std::string prefix = "{\"name\":\"" + std::string(128, 'n') + "\"";
    {
        auto supplied = ruvia::from_json<failing_default_model>(prefix + ",\"value\":9}", {.resource_ = &memory});
        EXPECT_EQ(supplied.get<"value">()->value_, 9);
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    EXPECT_EQ(memory.live_allocations(), std::size_t{0});
    for (std::size_t index = 0; index < 32; ++index) {
        bool failed = false;
        try {
            (void)ruvia::from_json<failing_default_model>(prefix + "}", {.resource_ = &memory});
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "default evaluation failed";
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(evaluations, index + 1);
        EXPECT_EQ(memory.live_allocations(), std::size_t{0});
    }
    EXPECT_TRUE(memory.allocation_count() > 0);
    EXPECT_EQ(memory.allocation_count(), memory.deallocation_count());
}

TEST(Ruvia, model_default_assignment_obeys_field_nullability) {
    evaluations = 0;
    auto null = ruvia::from_json<null_default>("{}");
    EXPECT_TRUE(null.is_null<"value">() && !null.is_present<"value">());
    auto supplied = ruvia::from_json<null_default>(R"({"value":9})");
    EXPECT_TRUE(!supplied.is_null<"value">() && supplied.get<"value">()->value_ == 9);
    auto dynamic = ruvia::from_json<nullable_dynamic_default>("{}");
    EXPECT_TRUE(dynamic.is_null<"value">() && !dynamic.is_present<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    auto valid = ruvia::from_json<nonnullable_dynamic_default>(R"({"value":1})");
    EXPECT_TRUE(!valid.is_null<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    bool rejected = false;
    try {
        (void)ruvia::from_json<nonnullable_dynamic_default>("{}");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_owned_default_is_normalized_to_the_model_resource) {
    evaluations = 0;
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    {
        owned_default_model retained({.resource_ = &destination});
        {
            auto parsed_value = ruvia::from_json<owned_default_model>("{}", {.resource_ = &source});
            EXPECT_EQ(evaluations, std::size_t{1});
            retained = std::move(parsed_value);
        }
        EXPECT_EQ(source.live_allocations(), std::size_t{0});
        EXPECT_EQ(evaluations, std::size_t{1});
        EXPECT_EQ(retained.get<"value">()->view(), std::string_view(std::string(128, 'd')));
        EXPECT_TRUE(!retained.is_present<"value">());
        retained.reset<"value">();
        EXPECT_EQ(destination.live_allocations(), std::size_t{0});
    }
    EXPECT_EQ(evaluations, std::size_t{1});
}
