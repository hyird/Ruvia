#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "ruvia/web/model.h"

#include "test_harness.h"

namespace {
RUVIA_MODEL(narrow_values,
    RUVIA_REQUIRED_FIELD(i8, ruvia::int8),
    RUVIA_REQUIRED_FIELD(u8, ruvia::uint8),
    RUVIA_REQUIRED_FIELD(i16, ruvia::int16),
    RUVIA_REQUIRED_FIELD(u16, ruvia::uint16),
    RUVIA_REQUIRED_FIELD(real, ruvia::double_value));
RUVIA_MODEL(narrow_nullable, RUVIA_OPTIONAL_FIELD(value, ruvia::uint8, RUVIA_NULLABLE));
RUVIA_MODEL(wide_values,
    RUVIA_OPTIONAL_FIELD(i32, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(u32, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(i64, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(u64, ruvia::uint64));
RUVIA_MODEL(floating_nullable,
    RUVIA_OPTIONAL_FIELD(single, ruvia::float_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(real, ruvia::double_value, RUVIA_NULLABLE));

struct convertible_real final {
    constexpr operator float() const noexcept {
        return 1.25f;
    }
    constexpr operator double() const noexcept {
        return 2.5;
    }
};
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
    RUVIA_CHECK_EQ(json.get<"i8">().value_, std::int8_t{-128});
    RUVIA_CHECK_EQ(json.get<"u16">().value_, std::uint16_t{65535});
    RUVIA_CHECK_EQ(json.get<"real">().value_, 1.5);
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<narrow_values>(R"({"i8":1.5,"u8":2,"i16":3,"u16":4,"real":1})");
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("128"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("-129"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("-1"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("256"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("32768"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("-32769"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("65536"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("-1"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::from_form<narrow_values>("i8=128&u8=255&i16=-32768&u16=65535&real=1.5");
    }));
    auto form = ruvia::from_form<narrow_values>("i8=-128&u8=255&i16=-32768&u16=65535&real=1.5");
    RUVIA_CHECK_EQ(form.get<"i8">().value_, std::int8_t{-128});
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
    bool rejected = false;
    try {
        parsed_value.set<"value">(ruvia::int64{-1});
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(parsed_value.is_present<"value">() && parsed_value.is_null<"value">());
}

RUVIA_TEST(model_integer_assignment_rejects_out_of_range_without_replacing_values) {
    wide_values model;
    model.set<"i32">(7).set<"u32">(7).set<"i64">(7).set<"u64">(7);
    const auto original = ruvia::to_json(model);
    const auto check_rejected = [&](auto assign) {
        bool rejected = false;
        try {
            assign();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK_EQ(ruvia::to_json(model), original);
    };
    check_rejected([&] { model.set<"i32">(std::int64_t{2147483648}); });
    check_rejected([&] { model.set<"i32">(std::int64_t{-2147483649}); });
    check_rejected([&] { model.set<"i32">(ruvia::uint64{4294967296}); });
    check_rejected([&] { model.set<"u32">(-1); });
    check_rejected([&] { model.set<"u32">(std::uint64_t{4294967296}); });
    check_rejected([&] { model.set<"u32">(ruvia::int64{-1}); });
    check_rejected([&] { model.set<"i64">((std::numeric_limits<std::uint64_t>::max)()); });
    check_rejected([&] { model.set<"i64">(ruvia::uint64{(std::numeric_limits<std::uint64_t>::max)()}); });
    check_rejected([&] { model.set<"u64">(-1); });
    check_rejected([&] { model.set<"u64">(ruvia::int64{-1}); });
}

RUVIA_TEST(model_floating_construction_rejects_finite_overflow_before_narrowing) {
    const auto check_rejected = [&](auto construct) {
        bool rejected = false;
        try {
            construct();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    };
    const double above_float_max = std::nextafter(
        static_cast<double>((std::numeric_limits<float>::max)()), std::numeric_limits<double>::infinity());
    check_rejected([&] { (void)ruvia::float_value(above_float_max); });
    check_rejected([&] { (void)ruvia::float_value(ruvia::double_value{-above_float_max}); });
    const long double above_float_max_extended = std::nextafter(
        static_cast<long double>((std::numeric_limits<float>::max)()), std::numeric_limits<long double>::infinity());
    check_rejected([&] { (void)ruvia::float_value(above_float_max_extended); });
    if constexpr ((std::numeric_limits<long double>::max)() > (std::numeric_limits<double>::max)()) {
        const long double above_double_max = std::nextafter(
            static_cast<long double>((std::numeric_limits<double>::max)()), std::numeric_limits<long double>::infinity());
        check_rejected([&] { (void)ruvia::double_value(above_double_max); });
        check_rejected([&] { (void)ruvia::double_value(-above_double_max); });
    }
}

RUVIA_TEST(model_floating_assignment_rejects_overflow_without_replacing_values_or_null) {
    auto value = ruvia::from_json<floating_nullable>(R"({"single":7,"real":9})");
    auto null_value = ruvia::from_json<floating_nullable>(R"({"single":null,"real":null})");
    const auto check_rejected = [&](auto assign) {
        for (auto* model : {&value, &null_value}) {
            bool rejected = false;
            try {
                assign(*model);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
        RUVIA_CHECK_EQ(value.get<"single">()->value_, 7.0f);
        RUVIA_CHECK_EQ(value.get<"real">()->value_, 9.0);
        RUVIA_CHECK(!value.is_null<"single">() && !value.is_null<"real">());
        RUVIA_CHECK(value.is_present<"single">() && value.is_present<"real">());
        RUVIA_CHECK(null_value.is_null<"single">() && null_value.is_null<"real">());
        RUVIA_CHECK(null_value.is_present<"single">() && null_value.is_present<"real">());
        RUVIA_CHECK_EQ(ruvia::to_json(null_value), R"({"single":null,"real":null})");
    };
    check_rejected([](auto& model) { model.template set<"single">((std::numeric_limits<double>::max)()); });
    check_rejected([](auto& model) { model.template set<"single">(ruvia::double_value{-(std::numeric_limits<double>::max)()}); });
    if constexpr ((std::numeric_limits<long double>::max)() > (std::numeric_limits<double>::max)()) {
        check_rejected([](auto& model) { model.template set<"real">((std::numeric_limits<long double>::max)()); });
    }
}

RUVIA_TEST(model_floating_construction_preserves_boundaries_rounding_and_scalar_inputs) {
    constexpr auto float_max = (std::numeric_limits<float>::max)();
    constexpr ruvia::float_value positive{static_cast<double>(float_max)};
    constexpr ruvia::float_value negative{ruvia::double_value{-static_cast<double>(float_max)}};
    constexpr ruvia::float_value rounded{1.0 + std::numeric_limits<float>::epsilon() / 4.0};
    constexpr ruvia::double_value double_boundary{static_cast<long double>((std::numeric_limits<double>::max)())};
    constexpr ruvia::double_value double_rounded{1.0L + std::numeric_limits<double>::epsilon() / 4.0L};
    RUVIA_CHECK_EQ(positive.value_, float_max);
    RUVIA_CHECK_EQ(negative.value_, -float_max);
    RUVIA_CHECK_EQ(rounded.value_, 1.0f);
    RUVIA_CHECK_EQ(double_boundary.value_, (std::numeric_limits<double>::max)());
    RUVIA_CHECK_EQ(double_rounded.value_, 1.0);
    RUVIA_CHECK_EQ(ruvia::float_value(17).value_, 17.0f);
    RUVIA_CHECK_EQ(ruvia::double_value(ruvia::int32{-17}).value_, -17.0);
    RUVIA_CHECK_EQ(ruvia::float_value(true).value_, 1.0f);
    RUVIA_CHECK_EQ(ruvia::double_value(ruvia::bool_value{false}).value_, 0.0);
    RUVIA_CHECK_EQ(ruvia::double_value(ruvia::float_value{1.25f}).value_, 1.25);
    constexpr ruvia::float_value custom_float{convertible_real{}};
    constexpr ruvia::double_value custom_double{convertible_real{}};
    RUVIA_CHECK_EQ(custom_float.value_, 1.25f);
    RUVIA_CHECK_EQ(custom_double.value_, 2.5);
}

RUVIA_TEST(model_floating_construction_and_assignment_preserve_explicit_nonfinite_inputs) {
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    RUVIA_CHECK_EQ(ruvia::float_value(infinity).value_, std::numeric_limits<float>::infinity());
    RUVIA_CHECK_EQ(ruvia::float_value(ruvia::double_value{-infinity}).value_, -std::numeric_limits<float>::infinity());
    RUVIA_CHECK(std::isnan(ruvia::float_value(ruvia::double_value{nan}).value_));
    RUVIA_CHECK_EQ(ruvia::double_value(std::numeric_limits<long double>::infinity()).value_, infinity);
    RUVIA_CHECK(std::isnan(ruvia::double_value(std::numeric_limits<long double>::quiet_NaN()).value_));
    floating_nullable model;
    model.set<"single">(ruvia::double_value{infinity});
    model.set<"real">(nan);
    RUVIA_CHECK(std::isinf(model.get<"single">()->value_));
    RUVIA_CHECK(std::isnan(model.get<"real">()->value_));
    RUVIA_CHECK(!model.is_null<"single">() && !model.is_null<"real">());
    RUVIA_CHECK_EQ(ruvia::to_json(model), R"({"single":null,"real":null})");
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
