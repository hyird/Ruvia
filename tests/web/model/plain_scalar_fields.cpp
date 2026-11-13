#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"

#include "test_exceptions.h"

RUVIA_MODEL(wrapped_scalars, RUVIA_OPTIONAL_FIELD(count, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value), RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::int64));

RUVIA_MODEL(wrapped_scalars_response, RUVIA_OPTIONAL_FIELD(count, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value), RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::int64));

RUVIA_MODEL(
    wrapped_defaulted, RUVIA_OPTIONAL_FIELD(retries, ruvia::uint32, RUVIA_DEFAULT(3)));

TEST(Ruvia, model_wrapper_scalar_fields_parse_json_and_forms) {
    constexpr std::string_view body = R"({"count":36,"ratio":9.5,"enabled":true,"delta":-7})";

    const auto wrapped = ruvia::from_json<wrapped_scalars>(body);
    EXPECT_EQ(std::uint32_t(wrapped.get<"count">().value()), std::uint32_t{36});
    EXPECT_TRUE(double(wrapped.get<"ratio">().value()) == 9.5);
    EXPECT_TRUE(bool(wrapped.get<"enabled">().value()));
    EXPECT_EQ(std::int64_t(wrapped.get<"delta">().value()), std::int64_t{-7});

    const auto form = ruvia::from_form<wrapped_scalars>(
        "count=41&ratio=2.5&enabled=true&delta=-9");
    EXPECT_EQ(std::uint32_t(form.get<"count">().value()), std::uint32_t{41});
    EXPECT_TRUE(double(form.get<"ratio">().value()) == 2.5);
    EXPECT_TRUE(bool(form.get<"enabled">().value()));
}

TEST(Ruvia, model_wrapper_scalar_fields_reject_mistyped_values_but_allow_missing_optional_fields) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":"nan"})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":null})");
    }));

    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>("{}");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_form<wrapped_scalars>("count=not-a-number");
    }));
}

TEST(Ruvia, model_wrapper_scalar_fields_round_trip_through_json) {
    const auto parsed_value = ruvia::from_json<wrapped_scalars>(
        R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})");

    wrapped_scalars_response response;
    response.set<"count">(*parsed_value.get<"count">())
        .set<"ratio">(*parsed_value.get<"ratio">())
        .set<"enabled">(*parsed_value.get<"enabled">())
        .set<"delta">(*parsed_value.get<"delta">());
    const auto json = ruvia::to_json(response);
    EXPECT_EQ(std::string_view(json),
        std::string_view(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})"));
}

TEST(Ruvia, model_wrapper_scalar_fields_apply_defaults) {
    const auto defaulted = ruvia::from_json<wrapped_defaulted>("{}");
    EXPECT_EQ(std::uint32_t(defaulted.get<"retries">().value()), std::uint32_t{3});
}
