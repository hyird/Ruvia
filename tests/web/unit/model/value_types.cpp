#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "ruvia/web/model.h"
#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/model_types.h"

#include "test_harness.h"

namespace {
RUVIA_MODEL(narrow_values,
    RUVIA_REQUIRED_FIELD(i8, ruvia::int8),
    RUVIA_REQUIRED_FIELD(u8, ruvia::uint8),
    RUVIA_REQUIRED_FIELD(i16, ruvia::int16),
    RUVIA_REQUIRED_FIELD(u16, ruvia::uint16),
    RUVIA_REQUIRED_FIELD(real, ruvia::double_value));
RUVIA_MODEL(narrow_nullable, RUVIA_OPTIONAL_FIELD(value, ruvia::uint8, RUVIA_NULLABLE));
}  // namespace

RUVIA_TEST(model_narrow_value_types_preserve_boundaries) {
    RUVIA_CHECK_EQ(ruvia::int8(-128).value_, std::int8_t{-128});
    RUVIA_CHECK_EQ(ruvia::uint8(255).value_, std::uint8_t{255});
    RUVIA_CHECK_EQ(ruvia::int16(-32768).value_, std::int16_t{-32768});
    RUVIA_CHECK_EQ(ruvia::uint16(65535).value_, std::uint16_t{65535});
    bool rejected = false;
    try {
        (void)ruvia::int8(128);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::uint8(-1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::int16(32768);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    rejected = false;
    try {
        (void)ruvia::uint16(65536);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(model_narrow_values_parse_json_form_and_reject_float_as_integer) {
    auto json = ruvia::from_json<narrow_values>(R"({"i8":-128,"u8":255,"i16":-32768,"u16":65535,"real":1.5})");
    RUVIA_CHECK(json.has_value());
    if (json) {
        RUVIA_CHECK_EQ(json->get<"i8">().value_, std::int8_t{-128});
        RUVIA_CHECK_EQ(json->get<"u16">().value_, std::uint16_t{65535});
        RUVIA_CHECK_EQ(json->get<"real">().value_, 1.5);
    }
    RUVIA_CHECK(!ruvia::from_json<narrow_values>(R"({"i8":1.5,"u8":2,"i16":3,"u16":4,"real":1})"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::int8>("128"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::int8>("-129"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::uint8>("-1"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::uint8>("256"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::int16>("32768"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::int16>("-32769"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::uint16>("65536"));
    RUVIA_CHECK(!ruvia::from_json<ruvia::uint16>("-1"));
    RUVIA_CHECK(!ruvia::from_form<narrow_values>("i8=128&u8=255&i16=-32768&u16=65535&real=1.5"));
    auto form = ruvia::from_form<narrow_values>("i8=-128&u8=255&i16=-32768&u16=65535&real=1.5");
    RUVIA_CHECK(form.has_value());
    if (form) {
        RUVIA_CHECK_EQ(form->get<"i8">().value_, std::int8_t{-128});
    }
}

RUVIA_TEST(model_narrow_assignment_checks_native_and_model_scalars_before_replacing_values) {
    narrow_nullable model;
    model.set<"value">(7);
    const auto check_rejected = [&](auto input) {
        bool rejected = false;
        try {
            model.set<"value">(input);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(model.get<"value">()->value_, std::uint8_t{7});
        RUVIA_CHECK(!model.is_null<"value">());
        RUVIA_CHECK(!model.is_present<"value">());
    };
    check_rejected(-1);
    check_rejected(256);
    check_rejected(ruvia::int32{-1});
    check_rejected(ruvia::uint64{256});
    auto parsed_value = ruvia::from_json<narrow_nullable>(R"({"value":null})");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    bool rejected = false;
    try {
        parsed_value->set<"value">(ruvia::int64{-1});
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(parsed_value->is_present<"value">() && parsed_value->is_null<"value">());
}

RUVIA_TEST(model_bytes_own_and_compare_without_allocator_identity) {
    std::byte input[] = {std::byte{0x01}, std::byte{0x02}};
    const auto bytes_value = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input), 2);
    std::pmr::monotonic_buffer_resource first_resource;
    std::pmr::monotonic_buffer_resource second_resource;
    ruvia::bytes first(bytes_value, {.resource_ = &first_resource});
    ruvia::bytes second(bytes_value, {.resource_ = &second_resource});

    RUVIA_CHECK(first == second);
    first.assign_owned(std::span<const std::uint8_t>{});
    RUVIA_CHECK(first.empty());
}

RUVIA_TEST(model_array_and_boxed_array_compare_values_in_order) {
    ruvia::array<ruvia::int16> first;
    ruvia::array<ruvia::int16> second;
    first.emplace_back(1);
    first.emplace_back(2);
    second.emplace_back(1);
    second.emplace_back(2);
    RUVIA_CHECK(first == second);
    second[1] = ruvia::int16(3);
    RUVIA_CHECK(!(first == second));
}
