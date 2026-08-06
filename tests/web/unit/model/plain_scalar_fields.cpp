#include "test_harness.h"

// A model field may be declared with a standard arithmetic type instead of the
// ruvia::Int32 family. The two forms must behave identically -- same parse
// results, same missing/invalid handling, same defaults, same serialization --
// so that choosing the plain type is a notational choice and nothing more.

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

struct WrappedScalars final {
    RUVIA_OPTIONAL_FIELD(count, ruvia::UInt32);
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::Double);
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool);
    RUVIA_OPTIONAL_FIELD(delta, ruvia::Int64);
    RUVIA_MODEL(WrappedScalars, count, ratio, enabled, delta);
};

struct PlainScalars final {
    RUVIA_OPTIONAL_FIELD(count, std::uint32_t);
    RUVIA_OPTIONAL_FIELD(ratio, double);
    RUVIA_OPTIONAL_FIELD(enabled, bool);
    RUVIA_OPTIONAL_FIELD(delta, std::int64_t);
    RUVIA_MODEL(PlainScalars, count, ratio, enabled, delta);
};

struct PlainDefaulted final {
    RUVIA_OPTIONAL_FIELD(retries, std::uint32_t, RUVIA_DEFAULT(3));
    RUVIA_MODEL(PlainDefaulted, retries);
};

struct PlainValidated final {
    RUVIA_OPTIONAL_FIELD(port, std::uint32_t);
    RUVIA_MODEL(PlainValidated, port);
};

namespace {

// Both declaration forms are recognized as model scalars, and neither drags in
// character types, which carry text rather than numbers.
static_assert(ruvia::detail::isRuviaScalar<ruvia::UInt32>);
static_assert(ruvia::detail::isRuviaScalar<std::uint32_t>);
static_assert(ruvia::detail::isRuviaScalar<bool>);
static_assert(ruvia::detail::isRuviaScalar<double>);
static_assert(!ruvia::detail::isRuviaScalar<char>);
static_assert(!ruvia::detail::isRuviaScalar<unsigned char>);
static_assert(!ruvia::detail::isRuviaScalar<ruvia::String>);

// Only the wrapper form carries a .value member to unwrap.
static_assert(ruvia::detail::isWrappedModelScalar<ruvia::UInt32>);
static_assert(!ruvia::detail::isWrappedModelScalar<std::uint32_t>);

// Both resolve to the same arithmetic type.
static_assert(std::is_same_v<ruvia::detail::ModelScalarValueT<ruvia::UInt32>, std::uint32_t>);
static_assert(std::is_same_v<ruvia::detail::ModelScalarValueT<std::uint32_t>, std::uint32_t>);

}  // namespace

RUVIA_TEST(model_plain_scalar_fields_parse_like_their_wrappers) {
    constexpr std::string_view body = R"({"count":36,"ratio":9.5,"enabled":true,"delta":-7})";

    std::pmr::monotonic_buffer_resource wrappedResource;
    const auto wrapped = ruvia::JsonBody<WrappedScalars>::parse(body, &wrappedResource);
    std::pmr::monotonic_buffer_resource plainResource;
    const auto plain = ruvia::JsonBody<PlainScalars>::parse(body, &plainResource);

    RUVIA_CHECK(wrapped.has_value());
    RUVIA_CHECK(plain.has_value());
    if (!wrapped || !plain) {
        return;
    }

    RUVIA_CHECK_EQ(std::uint32_t(wrapped->count().value()), std::uint32_t{36});
    RUVIA_CHECK_EQ(std::uint32_t(plain->count().value()), std::uint32_t{36});
    RUVIA_CHECK(double(plain->ratio().value()) == 9.5);
    RUVIA_CHECK(bool(plain->enabled().value()));
    RUVIA_CHECK_EQ(std::int64_t(plain->delta().value()), std::int64_t{-7});
}

RUVIA_TEST(model_plain_scalar_fields_treat_missing_and_mistyped_alike) {
    // A value of the wrong JSON type leaves the field unset rather than failing
    // the document -- the same split the wrapper form produces.
    for (const auto body : {std::string_view(R"({"count":"nan"})"), std::string_view("{}"), std::string_view(R"({"count":null})")}) {
        std::pmr::monotonic_buffer_resource wrappedResource;
        const auto wrapped = ruvia::JsonBody<WrappedScalars>::parse(body, &wrappedResource);
        std::pmr::monotonic_buffer_resource plainResource;
        const auto plain = ruvia::JsonBody<PlainScalars>::parse(body, &plainResource);

        RUVIA_CHECK(wrapped.has_value());
        RUVIA_CHECK(plain.has_value());
        if (!wrapped || !plain) {
            continue;
        }
        RUVIA_CHECK_EQ(wrapped->count().has_value(), plain->count().has_value());
        RUVIA_CHECK(!plain->count().has_value());
    }
}

RUVIA_TEST(model_plain_scalar_fields_round_trip_through_json) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::JsonBody<PlainScalars>::parse(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})", &resource);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    // Serialization emits the value directly; there is no wrapper to unwrap.
    const auto json = ruvia::toJson(*parsed, &resource);
    RUVIA_CHECK_EQ(std::string_view(json), std::string_view(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})"));
}

RUVIA_TEST(model_plain_scalar_fields_parse_forms_and_apply_defaults) {
    std::pmr::monotonic_buffer_resource formResource;
    const auto form = ruvia::FormBody<PlainScalars>::parse("count=41&ratio=2.5&enabled=true&delta=-9", &formResource);
    RUVIA_CHECK(form.has_value());
    if (form) {
        RUVIA_CHECK_EQ(std::uint32_t(form->count().value()), std::uint32_t{41});
        RUVIA_CHECK(double(form->ratio().value()) == 2.5);
        RUVIA_CHECK(bool(form->enabled().value()));
    }

    std::pmr::monotonic_buffer_resource defaultResource;
    const auto defaulted = ruvia::JsonBody<PlainDefaulted>::parse("{}", &defaultResource);
    RUVIA_CHECK(defaulted.has_value());
    if (defaulted) {
        RUVIA_CHECK_EQ(std::uint32_t(defaulted->retries().value()), std::uint32_t{3});
    }
}
