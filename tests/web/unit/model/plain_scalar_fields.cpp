#include "test_harness.h"

// Model fields deliberately use Ruvia model value types. Plain arithmetic
// declarations are rejected at the trait layer so schema fields do not drift
// away from the public model contract.

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

RUVIA_REQUEST_MODEL(WrappedScalars,
    RUVIA_OPTIONAL_FIELD(count, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::Int64));

RUVIA_RESPONSE_MODEL(WrappedScalarsResponse,
    RUVIA_OPTIONAL_FIELD(count, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::Double),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::Int64));

RUVIA_REQUEST_MODEL(WrappedDefaulted,
    RUVIA_OPTIONAL_FIELD(retries, ruvia::UInt32, RUVIA_DEFAULT(3)));

namespace {

static_assert(ruvia::detail::isRuviaScalar<ruvia::UInt32>);
static_assert(ruvia::detail::isRuviaScalar<ruvia::Bool>);
static_assert(ruvia::detail::isRuviaScalar<ruvia::Double>);
static_assert(!ruvia::detail::isRuviaScalar<std::uint32_t>);
static_assert(!ruvia::detail::isRuviaScalar<bool>);
static_assert(!ruvia::detail::isRuviaScalar<double>);
static_assert(!ruvia::detail::isRuviaScalar<char>);
static_assert(!ruvia::detail::isRuviaScalar<unsigned char>);
static_assert(!ruvia::detail::isRuviaScalar<ruvia::String>);

static_assert(std::is_same_v<ruvia::detail::ModelScalarValueT<ruvia::UInt32>, std::uint32_t>);
static_assert(ruvia::detail::isRequestModelField<ruvia::UInt32>);
static_assert(ruvia::detail::isRequestModelField<ruvia::Array<ruvia::UInt32>>);
static_assert(!ruvia::detail::isRequestModelField<std::uint32_t>);
static_assert(!ruvia::detail::isRequestModelField<bool>);
static_assert(!ruvia::detail::isRequestModelField<double>);
static_assert(!ruvia::detail::isRequestModelField<std::string>);
static_assert(!ruvia::detail::isRequestModelField<std::string_view>);
static_assert(!ruvia::detail::isRequestModelField<ruvia::Array<std::uint32_t>>);

}  // namespace

RUVIA_TEST(model_wrapper_scalar_fields_parse_json_and_forms) {
    constexpr std::string_view body = R"({"count":36,"ratio":9.5,"enabled":true,"delta":-7})";

    std::pmr::monotonic_buffer_resource wrappedResource;
    const auto wrapped = ruvia::fromJson<WrappedScalars>(body, {.resource = &wrappedResource});
    RUVIA_CHECK(wrapped.has_value());
    if (!wrapped) {
        return;
    }

    RUVIA_CHECK_EQ(std::uint32_t(wrapped->get<"count">().value()), std::uint32_t{36});
    RUVIA_CHECK(double(wrapped->get<"ratio">().value()) == 9.5);
    RUVIA_CHECK(bool(wrapped->get<"enabled">().value()));
    RUVIA_CHECK_EQ(std::int64_t(wrapped->get<"delta">().value()), std::int64_t{-7});

    std::pmr::monotonic_buffer_resource formResource;
    const auto form = ruvia::fromForm<WrappedScalars>("count=41&ratio=2.5&enabled=true&delta=-9", {.resource = &formResource});
    RUVIA_CHECK(form.has_value());
    if (form) {
        RUVIA_CHECK_EQ(std::uint32_t(form->get<"count">().value()), std::uint32_t{41});
        RUVIA_CHECK(double(form->get<"ratio">().value()) == 2.5);
        RUVIA_CHECK(bool(form->get<"enabled">().value()));
    }
}

RUVIA_TEST(model_wrapper_scalar_fields_reject_mistyped_values_but_allow_missing_optional_fields) {
    for (const auto body : {std::string_view(R"({"count":"nan"})"), std::string_view(R"({"count":null})")}) {
        std::pmr::monotonic_buffer_resource wrappedResource;
        const auto wrapped = ruvia::fromJson<WrappedScalars>(body, {.resource = &wrappedResource});
        RUVIA_CHECK(!wrapped.has_value());
    }

    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(ruvia::fromJson<WrappedScalars>("{}", {.resource = &resource}).has_value());

    RUVIA_CHECK(!ruvia::fromForm<WrappedScalars>("count=not-a-number", {.resource = &resource}).has_value());
    const auto partialForm = ruvia::detail::ModelParseAccess::parseFormBorrowedPartial<WrappedScalars>("count=not-a-number", &resource);
    RUVIA_CHECK(partialForm.has_value());
    if (partialForm) {
        RUVIA_CHECK(
            ruvia::detail::ModelValidationAccess::fieldState<"count">(*partialForm) ==
            ruvia::detail::ModelFieldState::kInvalidType);
    }
}

RUVIA_TEST(model_wrapper_scalar_fields_round_trip_through_json) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::fromJson<WrappedScalars>(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    WrappedScalarsResponse response({.resource = &resource});
    response.set<"count">(*parsed->get<"count">())
        .set<"ratio">(*parsed->get<"ratio">())
        .set<"enabled">(*parsed->get<"enabled">())
        .set<"delta">(*parsed->get<"delta">());
    const auto json = ruvia::toJson(response, {.resource = &resource});
    RUVIA_CHECK_EQ(std::string_view(json), std::string_view(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})"));
}

RUVIA_TEST(model_wrapper_scalar_fields_apply_defaults) {
    std::pmr::monotonic_buffer_resource defaultResource;
    const auto defaulted = ruvia::fromJson<WrappedDefaulted>("{}", {.resource = &defaultResource});
    RUVIA_CHECK(defaulted.has_value());
    if (defaulted) {
        RUVIA_CHECK_EQ(std::uint32_t(defaulted->get<"retries">().value()), std::uint32_t{3});
    }
}
