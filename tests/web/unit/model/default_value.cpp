#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

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
    auto value = ruvia::json_value::parse("null");
    return std::move(*value);
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
RUVIA_MODEL(required_default,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32, RUVIA_DEFAULT(evaluated_default())));
RUVIA_MODEL(failing_default_model,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_DEFAULT(failing_default_value())));
RUVIA_MODEL(owned_default_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(owned_default_value())));

}  // namespace

RUVIA_TEST(model_default_expression_is_not_evaluated_for_supplied_or_invalid_input) {
    evaluations = 0;
    optional_default constructed;
    RUVIA_CHECK(!constructed.get<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    for (auto input : {R"({"value":9})", R"({"value":null})", R"({"value":"wrong"})",
             R"({"value":8,"value":9})", R"({"value":)"}) {
        auto parsed_value = ruvia::from_json<optional_default>(input);
        RUVIA_CHECK_EQ(evaluations, std::size_t{0});
        if (input == std::string_view(R"({"value":9})")) {
            RUVIA_CHECK(parsed_value && parsed_value->get<"value">()->value_ == 9);
        } else {
            RUVIA_CHECK(!parsed_value);
        }
    }
    auto null = ruvia::from_json<nullable_default>(R"({"value":null})");
    RUVIA_CHECK(null && null->is_null<"value">());
    RUVIA_CHECK(!ruvia::from_json<required_default>("{}"));
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    RUVIA_CHECK(ruvia::from_form<optional_default>("value=9").has_value());
    RUVIA_CHECK(!ruvia::from_form<optional_default>("value=wrong"));
    RUVIA_CHECK(!ruvia::from_form<optional_default>("value=8&value=9"));
    RUVIA_CHECK(!ruvia::from_form<required_default>(""));
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
}

RUVIA_TEST(model_default_expression_runs_once_for_absence_not_for_moves) {
    evaluations = 0;
    auto parsed_value = ruvia::from_json<optional_default>("{}");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK(!parsed_value->is_present<"value">());
    RUVIA_CHECK_EQ(parsed_value->get<"value">()->value_, std::int32_t{7});
    ruvia::validator validator;
    ruvia::detail::model_validation_access::validate_model(*parsed_value, validator);
    RUVIA_CHECK(validator.ok());
    optional_default moved(std::move(*parsed_value));
    optional_default assigned;
    assigned = std::move(moved);
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(assigned.get<"value">()->value_, std::int32_t{7});
    assigned.reset<"value">();
    RUVIA_CHECK(!assigned.get<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    const auto form = ruvia::from_form<optional_default>("");
    RUVIA_CHECK(form && !form->is_present<"value">() && form->get<"value">()->value_ == 7);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_default_expression_exceptions_unwind_partial_owned_fields) {
    evaluations = 0;
    ruvia::test::counting_memory_resource memory;
    const std::string prefix = "{\"name\":\"" + std::string(128, 'n') + "\"";
    {
        auto supplied = ruvia::from_json<failing_default_model>(prefix + ",\"value\":9}", {.resource_ = &memory});
        RUVIA_CHECK(supplied.has_value());
        RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    for (std::size_t index = 0; index < 32; ++index) {
        bool failed = false;
        try {
            (void)ruvia::from_json<failing_default_model>(prefix + "}", {.resource_ = &memory});
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "default evaluation failed";
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(evaluations, index + 1);
        RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(memory.allocation_count() > 0);
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(model_default_assignment_obeys_field_nullability) {
    evaluations = 0;
    auto null = ruvia::from_json<null_default>("{}");
    RUVIA_CHECK(null && null->is_null<"value">() && !null->is_present<"value">());
    auto supplied = ruvia::from_json<null_default>(R"({"value":9})");
    RUVIA_CHECK(supplied && !supplied->is_null<"value">() && supplied->get<"value">()->value_ == 9);
    auto dynamic = ruvia::from_json<nullable_dynamic_default>("{}");
    RUVIA_CHECK(dynamic && dynamic->is_null<"value">() && !dynamic->is_present<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    auto valid = ruvia::from_json<nonnullable_dynamic_default>(R"({"value":1})");
    RUVIA_CHECK(valid && !valid->is_null<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    bool rejected = false;
    try {
        (void)ruvia::from_json<nonnullable_dynamic_default>("{}");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_owned_default_is_normalized_to_the_model_resource) {
    evaluations = 0;
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    {
        owned_default_model retained({.resource_ = &destination});
        {
            auto parsed_value = ruvia::from_json<owned_default_model>("{}", {.resource_ = &source});
            RUVIA_CHECK(parsed_value.has_value());
            if (!parsed_value) {
                return;
            }
            RUVIA_CHECK_EQ(evaluations, std::size_t{1});
            RUVIA_CHECK(parsed_value->get<"value">()->resource() == &source);
            retained = std::move(*parsed_value);
        }
        RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(evaluations, std::size_t{1});
        RUVIA_CHECK(retained.get<"value">()->resource() == &destination);
        RUVIA_CHECK_EQ(retained.get<"value">()->view(), std::string_view(std::string(128, 'd')));
        RUVIA_CHECK(!retained.is_present<"value">());
        retained.reset<"value">();
        RUVIA_CHECK_EQ(destination.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
}
