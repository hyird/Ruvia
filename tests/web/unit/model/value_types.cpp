#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelForm.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelTypes.h"

#include "test_harness.h"

namespace {
RUVIA_MODEL(NarrowValues,
    RUVIA_REQUIRED_FIELD(i8, ruvia::Int8),
    RUVIA_REQUIRED_FIELD(u8, ruvia::UInt8),
    RUVIA_REQUIRED_FIELD(i16, ruvia::Int16),
    RUVIA_REQUIRED_FIELD(u16, ruvia::UInt16),
    RUVIA_REQUIRED_FIELD(real, ruvia::Double));
RUVIA_MODEL(NarrowNullable, RUVIA_OPTIONAL_FIELD(value, ruvia::UInt8, RUVIA_NULLABLE));
}  // namespace

RUVIA_TEST(model_narrow_value_types_preserve_boundaries) {
    RUVIA_CHECK_EQ(ruvia::Int8(-128).value, std::int8_t{-128});
    RUVIA_CHECK_EQ(ruvia::UInt8(255).value, std::uint8_t{255});
    RUVIA_CHECK_EQ(ruvia::Int16(-32768).value, std::int16_t{-32768});
    RUVIA_CHECK_EQ(ruvia::UInt16(65535).value, std::uint16_t{65535});
    bool rejected = false;
    try {
        (void)ruvia::Int8(128);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::UInt8(-1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::Int16(32768);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::UInt16(65536);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(model_narrow_values_parse_json_form_and_reject_float_as_integer) {
    auto json = ruvia::fromJson<NarrowValues>(R"({"i8":-128,"u8":255,"i16":-32768,"u16":65535,"real":1.5})");
    RUVIA_CHECK(json.has_value());
    if (json) {
        RUVIA_CHECK_EQ(json->get<"i8">().value, std::int8_t{-128});
        RUVIA_CHECK_EQ(json->get<"u16">().value, std::uint16_t{65535});
        RUVIA_CHECK_EQ(json->get<"real">().value, 1.5);
    }
    RUVIA_CHECK(!ruvia::fromJson<NarrowValues>(R"({"i8":1.5,"u8":2,"i16":3,"u16":4,"real":1})"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::Int8>("128"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::Int8>("-129"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::UInt8>("-1"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::UInt8>("256"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::Int16>("32768"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::Int16>("-32769"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::UInt16>("65536"));
    RUVIA_CHECK(!ruvia::fromJson<ruvia::UInt16>("-1"));
    RUVIA_CHECK(!ruvia::fromForm<NarrowValues>("i8=128&u8=255&i16=-32768&u16=65535&real=1.5"));
    auto form = ruvia::fromForm<NarrowValues>("i8=-128&u8=255&i16=-32768&u16=65535&real=1.5");
    RUVIA_CHECK(form.has_value());
    if (form) {
        RUVIA_CHECK_EQ(form->get<"i8">().value, std::int8_t{-128});
    }
}

RUVIA_TEST(model_narrow_assignment_checks_native_and_model_scalars_before_replacing_values) {
    NarrowNullable model;
    model.set<"value">(7);
    const auto checkRejected = [&](auto input) {
        bool rejected = false;
        try {
            model.set<"value">(input);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(model.get<"value">()->value, std::uint8_t{7});
        RUVIA_CHECK(!model.isNull<"value">());
        RUVIA_CHECK(!model.isPresent<"value">());
    };
    checkRejected(-1);
    checkRejected(256);
    checkRejected(ruvia::Int32{-1});
    checkRejected(ruvia::UInt64{256});
    auto parsed = ruvia::fromJson<NarrowNullable>(R"({"value":null})");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    bool rejected = false;
    try {
        parsed->set<"value">(ruvia::Int64{-1});
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(parsed->isPresent<"value">() && parsed->isNull<"value">());
}

RUVIA_TEST(model_bytes_own_and_compare_without_allocator_identity) {
    std::byte input[] = {std::byte{0x01}, std::byte{0x02}};
    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input), 2);
    std::pmr::monotonic_buffer_resource firstResource;
    std::pmr::monotonic_buffer_resource secondResource;
    ruvia::Bytes first(bytes, {.resource = &firstResource});
    ruvia::Bytes second(bytes, {.resource = &secondResource});

    RUVIA_CHECK(first == second);
    RUVIA_CHECK_EQ(first.resource(), &firstResource);
    RUVIA_CHECK_EQ(second.resource(), &secondResource);
    first.assignOwned(std::span<const std::uint8_t>{});
    RUVIA_CHECK(first.empty());
}

RUVIA_TEST(model_array_and_boxed_array_compare_values_in_order) {
    ruvia::Array<ruvia::Int16> first;
    ruvia::Array<ruvia::Int16> second;
    first.emplace_back(1);
    first.emplace_back(2);
    second.emplace_back(1);
    second.emplace_back(2);
    RUVIA_CHECK(first == second);
    second[1] = ruvia::Int16(3);
    RUVIA_CHECK(!(first == second));
}
