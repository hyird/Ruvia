#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/ModelForm.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

std::size_t evaluations{0};

int evaluatedDefault() {
    ++evaluations;
    return 7;
}

int failingDefault() {
    ++evaluations;
    throw std::runtime_error("default evaluation failed");
}

std::string ownedDefault() {
    ++evaluations;
    return std::string(128, 'd');
}

ruvia::JsonValue dynamicNullDefault() {
    ++evaluations;
    auto value = ruvia::JsonValue::parse("null");
    return std::move(*value);
}

RUVIA_MODEL(OptionalDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::Int32, RUVIA_DEFAULT(evaluatedDefault()), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(NullableDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::Int32, RUVIA_NULLABLE, RUVIA_DEFAULT(evaluatedDefault())));
RUVIA_MODEL(NullDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::Int32, RUVIA_NULLABLE, RUVIA_DEFAULT(nullptr)));
RUVIA_MODEL(NullableDynamicDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::JsonValue, RUVIA_NULLABLE, RUVIA_DEFAULT(dynamicNullDefault())));
RUVIA_MODEL(NonnullableDynamicDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::JsonValue, RUVIA_DEFAULT(dynamicNullDefault())));
RUVIA_MODEL(RequiredDefault,
    RUVIA_REQUIRED_FIELD(value, ruvia::Int32, RUVIA_DEFAULT(evaluatedDefault())));
RUVIA_MODEL(FailingDefault,
    RUVIA_REQUIRED_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(value, ruvia::Int32, RUVIA_DEFAULT(failingDefault())));
RUVIA_MODEL(OwnedDefault,
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_DEFAULT(ownedDefault())));

}  // namespace

RUVIA_TEST(model_default_expression_is_not_evaluated_for_supplied_or_invalid_input) {
    evaluations = 0;
    OptionalDefault constructed;
    RUVIA_CHECK(!constructed.get<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    for (auto input : {R"({"value":9})", R"({"value":null})", R"({"value":"wrong"})",
             R"({"value":8,"value":9})", R"({"value":)"}) {
        auto parsed = ruvia::fromJson<OptionalDefault>(input);
        RUVIA_CHECK_EQ(evaluations, std::size_t{0});
        if (input == std::string_view(R"({"value":9})")) {
            RUVIA_CHECK(parsed && parsed->get<"value">()->value == 9);
        } else {
            RUVIA_CHECK(!parsed);
        }
    }
    auto null = ruvia::fromJson<NullableDefault>(R"({"value":null})");
    RUVIA_CHECK(null && null->isNull<"value">());
    RUVIA_CHECK(!ruvia::fromJson<RequiredDefault>("{}"));
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    RUVIA_CHECK(ruvia::fromForm<OptionalDefault>("value=9").has_value());
    RUVIA_CHECK(!ruvia::fromForm<OptionalDefault>("value=wrong"));
    RUVIA_CHECK(!ruvia::fromForm<OptionalDefault>("value=8&value=9"));
    RUVIA_CHECK(!ruvia::fromForm<RequiredDefault>(""));
    RUVIA_CHECK_EQ(evaluations, std::size_t{0});
}

RUVIA_TEST(model_default_expression_runs_once_for_absence_not_for_moves) {
    evaluations = 0;
    auto parsed = ruvia::fromJson<OptionalDefault>("{}");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK(!parsed->isPresent<"value">());
    RUVIA_CHECK_EQ(parsed->get<"value">()->value, std::int32_t{7});
    ruvia::Validator validator;
    ruvia::detail::ModelValidationAccess::validateModel(*parsed, validator);
    RUVIA_CHECK(validator.ok());
    OptionalDefault moved(std::move(*parsed));
    OptionalDefault assigned;
    assigned = std::move(moved);
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    RUVIA_CHECK_EQ(assigned.get<"value">()->value, std::int32_t{7});
    assigned.reset<"value">();
    RUVIA_CHECK(!assigned.get<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    const auto form = ruvia::fromForm<OptionalDefault>("");
    RUVIA_CHECK(form && !form->isPresent<"value">() && form->get<"value">()->value == 7);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_default_expression_exceptions_unwind_partial_owned_fields) {
    evaluations = 0;
    ruvia::test::CountingMemoryResource memory;
    const std::string prefix = "{\"name\":\"" + std::string(128, 'n') + "\"";
    {
        auto supplied = ruvia::fromJson<FailingDefault>(prefix + ",\"value\":9}", {.resource = &memory});
        RUVIA_CHECK(supplied.has_value());
        RUVIA_CHECK_EQ(evaluations, std::size_t{0});
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    for (std::size_t index = 0; index < 32; ++index) {
        bool failed = false;
        try {
            (void)ruvia::fromJson<FailingDefault>(prefix + "}", {.resource = &memory});
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "default evaluation failed";
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(evaluations, index + 1);
        RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK(memory.allocationCount() > 0);
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(model_default_assignment_obeys_field_nullability) {
    evaluations = 0;
    auto null = ruvia::fromJson<NullDefault>("{}");
    RUVIA_CHECK(null && null->isNull<"value">() && !null->isPresent<"value">());
    auto supplied = ruvia::fromJson<NullDefault>(R"({"value":9})");
    RUVIA_CHECK(supplied && !supplied->isNull<"value">() && supplied->get<"value">()->value == 9);
    auto dynamic = ruvia::fromJson<NullableDynamicDefault>("{}");
    RUVIA_CHECK(dynamic && dynamic->isNull<"value">() && !dynamic->isPresent<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    auto valid = ruvia::fromJson<NonnullableDynamicDefault>(R"({"value":1})");
    RUVIA_CHECK(valid && !valid->isNull<"value">());
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
    bool rejected = false;
    try {
        (void)ruvia::fromJson<NonnullableDynamicDefault>("{}");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(evaluations, std::size_t{2});
}

RUVIA_TEST(model_owned_default_is_normalized_to_the_model_resource) {
    evaluations = 0;
    ruvia::test::CountingMemoryResource source;
    ruvia::test::CountingMemoryResource destination;
    {
        OwnedDefault retained({.resource = &destination});
        {
            auto parsed = ruvia::fromJson<OwnedDefault>("{}", {.resource = &source});
            RUVIA_CHECK(parsed.has_value());
            if (!parsed) {
                return;
            }
            RUVIA_CHECK_EQ(evaluations, std::size_t{1});
            RUVIA_CHECK(parsed->get<"value">()->resource() == &source);
            retained = std::move(*parsed);
        }
        RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
        RUVIA_CHECK_EQ(evaluations, std::size_t{1});
        RUVIA_CHECK(retained.get<"value">()->resource() == &destination);
        RUVIA_CHECK_EQ(retained.get<"value">()->view(), std::string_view(std::string(128, 'd')));
        RUVIA_CHECK(!retained.isPresent<"value">());
        retained.reset<"value">();
        RUVIA_CHECK_EQ(destination.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(evaluations, std::size_t{1});
}
