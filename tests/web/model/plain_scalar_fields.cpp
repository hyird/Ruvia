#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
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

    std::pmr::monotonic_buffer_resource wrapped_resource;
    const auto wrapped = ruvia::from_json<wrapped_scalars>(body, {.resource_ = &wrapped_resource});
    EXPECT_EQ(std::uint32_t(wrapped.get<"count">().value()), std::uint32_t{36});
    EXPECT_TRUE(double(wrapped.get<"ratio">().value()) == 9.5);
    EXPECT_TRUE(bool(wrapped.get<"enabled">().value()));
    EXPECT_EQ(std::int64_t(wrapped.get<"delta">().value()), std::int64_t{-7});

    std::pmr::monotonic_buffer_resource form_resource;
    const auto form = ruvia::from_form<wrapped_scalars>(
        "count=41&ratio=2.5&enabled=true&delta=-9", {.resource_ = &form_resource});
    EXPECT_EQ(std::uint32_t(form.get<"count">().value()), std::uint32_t{41});
    EXPECT_TRUE(double(form.get<"ratio">().value()) == 2.5);
    EXPECT_TRUE(bool(form.get<"enabled">().value()));
}

TEST(Ruvia, model_wrapper_scalar_fields_reject_mistyped_values_but_allow_missing_optional_fields) {
    std::pmr::monotonic_buffer_resource wrapped_resource;
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":"nan"})", {.resource_ = &wrapped_resource});
    }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":null})", {.resource_ = &wrapped_resource});
    }));

    std::pmr::monotonic_buffer_resource resource;
    EXPECT_TRUE(!ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>("{}", {.resource_ = &resource});
    }));

    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_form<wrapped_scalars>("count=not-a-number", {.resource_ = &resource});
    }));
}

TEST(Ruvia, model_wrapper_scalar_fields_round_trip_through_json) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::from_json<wrapped_scalars>(
        R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})", {.resource_ = &resource});

    wrapped_scalars_response response({.resource_ = &resource});
    response.set<"count">(*parsed_value.get<"count">())
        .set<"ratio">(*parsed_value.get<"ratio">())
        .set<"enabled">(*parsed_value.get<"enabled">())
        .set<"delta">(*parsed_value.get<"delta">());
    const auto json = ruvia::to_json(response, {.resource_ = &resource});
    EXPECT_EQ(std::string_view(json),
        std::string_view(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})"));
}

TEST(Ruvia, model_wrapper_scalar_fields_apply_defaults) {
    std::pmr::monotonic_buffer_resource default_resource;
    const auto defaulted = ruvia::from_json<wrapped_defaulted>("{}", {.resource_ = &default_resource});
    EXPECT_EQ(std::uint32_t(defaulted.get<"retries">().value()), std::uint32_t{3});
}
