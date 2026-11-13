#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/web/error.h"
#include "ruvia/web/model.h"

#include "test_exceptions.h"

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

TEST(Ruvia, model_narrow_value_types_preserve_boundaries) {
    EXPECT_EQ(ruvia::int8(-128).value_, std::int8_t{-128});
    EXPECT_EQ(ruvia::uint8(255).value_, std::uint8_t{255});
    EXPECT_EQ(ruvia::int16(-32768).value_, std::int16_t{-32768});
    EXPECT_EQ(ruvia::uint16(65535).value_, std::uint16_t{65535});
    bool rejected = false;
    try {
        (void)ruvia::int8(128);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::uint8(-1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::int16(32768);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::uint16(65536);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, model_narrow_values_parse_json_form_and_reject_float_as_integer) {
    auto json = ruvia::from_json<narrow_values>(R"({"i8":-128,"u8":255,"i16":-32768,"u16":65535,"real":1.5})");
    EXPECT_EQ(json.get<"i8">().value_, std::int8_t{-128});
    EXPECT_EQ(json.get<"u16">().value_, std::uint16_t{65535});
    EXPECT_EQ(json.get<"real">().value_, 1.5);
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<narrow_values>(R"({"i8":1.5,"u8":2,"i16":3,"u16":4,"real":1})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("128"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("-129"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("-1"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("256"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("32768"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("-32769"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("65536"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("-1"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_form<narrow_values>("i8=128&u8=255&i16=-32768&u16=65535&real=1.5");
    }));
    auto form = ruvia::from_form<narrow_values>("i8=-128&u8=255&i16=-32768&u16=65535&real=1.5");
    EXPECT_EQ(form.get<"i8">().value_, std::int8_t{-128});
}

TEST(Ruvia, model_narrow_assignment_checks_native_and_model_scalars_before_replacing_values) {
    narrow_nullable model;
    model.set<"value">(7);
    const auto check_rejected = [&](auto input) {
        bool rejected = false;
        try {
            model.set<"value">(input);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(model.get<"value">()->value_, std::uint8_t{7});
        EXPECT_TRUE(!model.is_null<"value">());
        EXPECT_TRUE(!model.is_present<"value">());
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
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(parsed_value.is_present<"value">() && parsed_value.is_null<"value">());
}

TEST(Ruvia, model_integer_assignment_rejects_out_of_range_without_replacing_values) {
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
        EXPECT_TRUE(rejected);
        EXPECT_EQ(ruvia::to_json(model), original);
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

TEST(Ruvia, model_floating_construction_rejects_finite_overflow_before_narrowing) {
    const auto check_rejected = [&](auto construct) {
        bool rejected = false;
        try {
            construct();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
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

TEST(Ruvia, model_floating_assignment_rejects_overflow_without_replacing_values_or_null) {
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
            EXPECT_TRUE(rejected);
        }
        EXPECT_EQ(value.get<"single">()->value_, 7.0f);
        EXPECT_EQ(value.get<"real">()->value_, 9.0);
        EXPECT_TRUE(!value.is_null<"single">() && !value.is_null<"real">());
        EXPECT_TRUE(value.is_present<"single">() && value.is_present<"real">());
        EXPECT_TRUE(null_value.is_null<"single">() && null_value.is_null<"real">());
        EXPECT_TRUE(null_value.is_present<"single">() && null_value.is_present<"real">());
        EXPECT_EQ(ruvia::to_json(null_value), R"({"single":null,"real":null})");
    };
    check_rejected([](auto& model) { model.template set<"single">((std::numeric_limits<double>::max)()); });
    check_rejected([](auto& model) { model.template set<"single">(ruvia::double_value{-(std::numeric_limits<double>::max)()}); });
    if constexpr ((std::numeric_limits<long double>::max)() > (std::numeric_limits<double>::max)()) {
        check_rejected([](auto& model) { model.template set<"real">((std::numeric_limits<long double>::max)()); });
    }
}

TEST(Ruvia, model_floating_construction_preserves_boundaries_rounding_and_scalar_inputs) {
    constexpr auto float_max = (std::numeric_limits<float>::max)();
    constexpr ruvia::float_value positive{static_cast<double>(float_max)};
    constexpr ruvia::float_value negative{ruvia::double_value{-static_cast<double>(float_max)}};
    constexpr ruvia::float_value rounded{1.0 + std::numeric_limits<float>::epsilon() / 4.0};
    constexpr ruvia::double_value double_boundary{static_cast<long double>((std::numeric_limits<double>::max)())};
    constexpr ruvia::double_value double_rounded{1.0L + std::numeric_limits<double>::epsilon() / 4.0L};
    EXPECT_EQ(positive.value_, float_max);
    EXPECT_EQ(negative.value_, -float_max);
    EXPECT_EQ(rounded.value_, 1.0f);
    EXPECT_EQ(double_boundary.value_, (std::numeric_limits<double>::max)());
    EXPECT_EQ(double_rounded.value_, 1.0);
    EXPECT_EQ(ruvia::float_value(17).value_, 17.0f);
    EXPECT_EQ(ruvia::double_value(ruvia::int32{-17}).value_, -17.0);
    EXPECT_EQ(ruvia::float_value(true).value_, 1.0f);
    EXPECT_EQ(ruvia::double_value(ruvia::bool_value{false}).value_, 0.0);
    EXPECT_EQ(ruvia::double_value(ruvia::float_value{1.25f}).value_, 1.25);
    constexpr ruvia::float_value custom_float{convertible_real{}};
    constexpr ruvia::double_value custom_double{convertible_real{}};
    EXPECT_EQ(custom_float.value_, 1.25f);
    EXPECT_EQ(custom_double.value_, 2.5);
}

TEST(Ruvia, model_floating_construction_and_assignment_preserve_explicit_nonfinite_inputs) {
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(ruvia::float_value(infinity).value_, std::numeric_limits<float>::infinity());
    EXPECT_EQ(ruvia::float_value(ruvia::double_value{-infinity}).value_, -std::numeric_limits<float>::infinity());
    EXPECT_TRUE(std::isnan(ruvia::float_value(ruvia::double_value{nan}).value_));
    EXPECT_EQ(ruvia::double_value(std::numeric_limits<long double>::infinity()).value_, infinity);
    EXPECT_TRUE(std::isnan(ruvia::double_value(std::numeric_limits<long double>::quiet_NaN()).value_));
    floating_nullable model;
    model.set<"single">(ruvia::double_value{infinity});
    model.set<"real">(nan);
    EXPECT_TRUE(std::isinf(model.get<"single">()->value_));
    EXPECT_TRUE(std::isnan(model.get<"real">()->value_));
    EXPECT_TRUE(!model.is_null<"single">() && !model.is_null<"real">());
    EXPECT_EQ(ruvia::to_json(model), R"({"single":null,"real":null})");
}

TEST(Ruvia, model_bytes_own_and_compare_without_allocator_identity) {
    std::byte input[] = {std::byte{0x01}, std::byte{0x02}};
    const auto bytes_value = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input), 2);
    ruvia::bytes first(bytes_value);
    ruvia::bytes second(bytes_value);

    EXPECT_TRUE(first == second);
    first.assign_owned(std::span<const std::uint8_t>{});
    EXPECT_TRUE(first.empty());
}

TEST(Ruvia, model_array_and_boxed_array_compare_values_in_order) {
    ruvia::array<ruvia::int16> first;
    ruvia::array<ruvia::int16> second;
    first.emplace_back(1);
    first.emplace_back(2);
    second.emplace_back(1);
    second.emplace_back(2);
    EXPECT_TRUE(first == second);
    second[1] = ruvia::int16(3);
    EXPECT_TRUE(!(first == second));
}

TEST(Ruvia, integer_decimal_limits) {
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<std::int64_t>("-9223372036854775808")), (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<std::uint64_t>("18446744073709551615")), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<int>("00042")), 42);
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<int>("-0")), 0);
    for (auto text : {"128", "-129"}) {
        EXPECT_EQ(std::get<1>(ruvia::parse_integer<std::int8_t>(text)), ruvia::integer_parse_error::out_of_range);
    }
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<std::uint64_t>("18446744073709551616")), ruvia::integer_parse_error::out_of_range);
}

TEST(Ruvia, integer_requires_complete_decimal_input) {
    for (auto text : {"", " ", "+1", " 1", "1 ", "1a", "0x10", "-", "1.0", "999999999999999999999999x"}) {
        EXPECT_EQ(std::get<1>(ruvia::parse_integer<int>(text)), ruvia::integer_parse_error::invalid_format);
    }
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<unsigned>("-1")), ruvia::integer_parse_error::invalid_format);
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<int>(std::string_view("12\0x", 4))), ruvia::integer_parse_error::invalid_format);
}

TEST(Ruvia, model_string_public_construction_owns_input) {
    std::string input(128, 'a');
    ruvia::string value(input);
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_owned_assignment_is_alias_safe) {
    ruvia::string value(std::string(128, 'd'));
    const auto alias = value.view().substr(31, 64);

    value.assign_owned(alias);
    const std::string expected(64, 'd');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_move_assignment_preserves_values_and_reclaims_storage) {
    const std::string source_text(128, 's');
    const std::string target_text(128, 't');
    ruvia::string source_value(source_text);
    ruvia::string target(target_text);

    target = std::move(source_value);
    EXPECT_EQ(target.view(), std::string_view(source_text));

    source_value.assign_owned(std::string(128, 'm'));
    const std::string moved_from_text(128, 'm');
    EXPECT_EQ(source_value.view(), std::string_view(moved_from_text));
}

TEST(Ruvia, model_string_assign_owned_rvalue_steals_storage) {
    const std::string expected(200, 'a');
    std::string input(expected);
    const char* const original_data = input.data();
    ruvia::string target;
    target.assign_owned(std::move(input));
    EXPECT_EQ(target.data(), original_data);
    EXPECT_EQ(target.view(), std::string_view(expected));
}

namespace {
RUVIA_MODEL(binary_model,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::bytes, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(binary_array_model,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::bytes>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::bytes>));

void check_bytes(const ruvia::bytes& actual, std::span<const std::uint8_t> expected) {
    EXPECT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::equal(actual.view().begin(), actual.view().end(), expected.begin()));
}
}  // namespace

TEST(Ruvia, model_json_bytes_roundtrip_empty_and_all_octets) {
    std::vector<std::uint8_t> octets(256);
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = ruvia::to_json(ruvia::bytes(std::span<const std::uint8_t>(octets)));
    EXPECT_TRUE(encoded.front() == '"' && encoded.back() == '"');
    EXPECT_TRUE(encoded.find("AAECAwQF") != std::string::npos);
    EXPECT_TRUE(encoded.find("/w==") != std::string::npos);
    auto parsed_value = ruvia::from_json<ruvia::bytes>(encoded);
    check_bytes(parsed_value, octets);
    auto empty = ruvia::from_json<ruvia::bytes>(R"("")");
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(std::string_view(ruvia::to_json(ruvia::bytes{})), R"("")");
}

TEST(Ruvia, model_json_bytes_accepts_padding_and_rejects_noncanonical_inputs) {
    auto one = ruvia::from_json<ruvia::bytes>(R"("Zg==")");
    auto two = ruvia::from_json<ruvia::bytes>(R"("Zm8=")");
    EXPECT_TRUE(one.size() == 1 && two.size() == 2);
    EXPECT_EQ(one.view().front(), std::uint8_t{0x66});
    EXPECT_EQ(two.view().back(), std::uint8_t{0x6f});
    auto escaped = ruvia::from_json<ruvia::bytes>(R"("\u005ag\u003d=")");
    EXPECT_EQ(escaped.view().front(), std::uint8_t{0x66});
    for (const auto input : {R"("Zg")", R"("Zh==")", R"("Zm=9")", R"("Zm9v=")",
             R"("Zm-v")", R"("Zm_v")", R"("Zm9v\n")", R"("Zm9=")", R"("====")",
             R"("Zg==AAAA")", R"("AA== ")", "null", "[1]", "123"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::bytes>(input); }));
    }
}

TEST(Ruvia, model_json_bytes_supports_nested_arrays_nullable_and_omit_empty) {
    auto nested = ruvia::from_json<binary_array_model>(R"({"items":["AQI=",""],"boxed":["/w=="]})");
    EXPECT_EQ(nested.get<"items">().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"items">()[0].view().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"boxed">().front().view().front(), std::uint8_t{255});
    auto model = ruvia::from_json<binary_model>(R"({"bytes":"AQI=","optional":null,"empty":""})");
    EXPECT_TRUE(model.is_null<"optional">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), R"({"bytes":"AQI=","optional":null})");
}

TEST(Ruvia, model_bytes_set_and_move_preserve_values) {
    binary_model model;
    const std::uint8_t raw[] = {1, 2, 3};
    model.set<"bytes">(std::span<const std::uint8_t>(raw));
    std::vector<std::uint8_t> owned({4, 5});
    model.set<"bytes">(std::move(owned));
    EXPECT_EQ(model.get<"bytes">().view()[1], std::uint8_t{5});
}

TEST(Ruvia, model_bytes_repeated_operations_reclaim_storage_and_keep_results) {
    {
        binary_model retained;
        {
            {
                std::vector<std::uint8_t> input(256, 0xff);
                retained.set<"bytes">(std::move(input));
            }
        }
        const auto saved = ruvia::to_json(retained.get<"bytes">());
        for (int i = 0; i < 32; ++i) {
            {
                auto parsed_value = ruvia::from_json<ruvia::bytes>(saved);
                EXPECT_TRUE(parsed_value == retained.get<"bytes">());
                const auto output = ruvia::to_json(parsed_value);
                EXPECT_EQ(output, saved);
                EXPECT_TRUE(ruvia::test::throws_on([&] {
                    (void)ruvia::from_json<ruvia::array<ruvia::bytes>>(
                        R"(["AQIDBA==","Zh=="])");
                }));
            }
            EXPECT_EQ(retained.get<"bytes">().view().front(), std::uint8_t{255});
        }
        std::vector<std::uint8_t> compatible(128, 7);
        retained.set<"bytes">(std::move(compatible));
        retained.ensure<"bytes">().assign_owned(retained.get<"bytes">().view().subspan(1));
        EXPECT_EQ(retained.get<"bytes">().size(), std::size_t{127});
        EXPECT_EQ(retained.get<"bytes">().view().back(), std::uint8_t{7});
        EXPECT_TRUE(saved.size() > 256);
    }
}

namespace {

class tracked_value final {
public:
    explicit tracked_value(int value) noexcept
        : value_(value) {
        ++alive_;
    }

    tracked_value(const tracked_value&) = delete;
    tracked_value& operator=(const tracked_value&) = delete;

    tracked_value(tracked_value&& other) noexcept
        : value_(other.value_) {
        other.value_ = -1;
        ++alive_;
    }

    tracked_value& operator=(tracked_value&&) = delete;

    ~tracked_value() {
        --alive_;
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    [[nodiscard]] static std::size_t alive() noexcept {
        return alive_;
    }

private:
    int value_;
    static inline std::size_t alive_{0};
};

}  // namespace

TEST(Ruvia, model_list_clear_and_destructor_release_owned_elements) {
    {
        ruvia::boxed_array<tracked_value> values;
        values.emplace_back(1);
        values.emplace_back(2);
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        values.clear();
        EXPECT_TRUE(values.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{0});

        values.emplace_back(3);
        EXPECT_EQ(tracked_value::alive(), std::size_t{1});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
}

TEST(Ruvia, model_list_move_assignment_keeps_element_resource_owner) {
    {
        ruvia::boxed_array<tracked_value> source_value;
        source_value.emplace_back(4);
        source_value.emplace_back(5);

        ruvia::boxed_array<tracked_value> target;
        target.emplace_back(9);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});

        target = std::move(source_value);
        EXPECT_EQ(target.size(), std::size_t{2});
        EXPECT_EQ(target[0].value(), 4);
        EXPECT_EQ(target[1].value(), 5);
        EXPECT_TRUE(source_value.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        source_value.emplace_back(6);
        EXPECT_EQ(source_value.front().value(), 6);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
}

TEST(Ruvia, model_boxed_array_supports_standard_range_algorithms) {
    ruvia::boxed_array<tracked_value> values;
    EXPECT_TRUE(std::ranges::find_if(values, [](const auto&) { return true; }) == values.end());
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{0});
    values.emplace_back(1);
    values.emplace_back(2);
    values.emplace_back(3);

    const auto found = std::ranges::find_if(values, [](const auto& value) { return value.value() == 2; });
    EXPECT_TRUE(found != values.end());
    if (found != values.end()) {
        EXPECT_TRUE(&*found == &values[1]);
    }
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{3});
    const auto& constant_values = std::as_const(values);
    EXPECT_EQ(std::ranges::count_if(constant_values,
                  [](const auto& value) { return value.value() % 2 != 0; }),
        std::ptrdiff_t{2});
    EXPECT_TRUE(std::ranges::find_if(constant_values,
                    [](const auto& value) { return value.value() == 4; }) == constant_values.end());
}

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

namespace {

RUVIA_MODEL(comparable_model,
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(comparable_tree,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<comparable_model>),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<comparable_tree>));

}  // namespace

TEST(Ruvia, model_equality_compares_state_and_value_but_not_presence) {
    comparable_model missing;
    comparable_model expected_value;
    expected_value.set<"number">(7);
    EXPECT_TRUE(missing != expected_value);

    auto parsed_value = ruvia::from_json<comparable_model>(R"({"number":7})");
    EXPECT_TRUE(parsed_value.is_present<"number">());
    EXPECT_TRUE(parsed_value == expected_value);

    expected_value.set<"number">(8);
    EXPECT_TRUE(parsed_value != expected_value);
    expected_value.set<"number">(nullptr);
    EXPECT_TRUE(parsed_value != expected_value);
}

TEST(Ruvia, model_equality_distinguishes_missing_and_null) {
    comparable_model missing;
    comparable_model null_value;
    null_value.set<"number">(nullptr);
    EXPECT_TRUE(missing != null_value);

    missing.set<"number">(nullptr);
    EXPECT_TRUE(missing == null_value);
}

TEST(Ruvia, model_equality_ignores_resources_for_normalized_values) {
    comparable_model left;
    comparable_model right;
    left.set<"text">(std::string_view("same value"));
    right.set<"text">(std::string_view("same value"));
    EXPECT_TRUE(left == right);

    right.set<"text">(std::string_view("different"));
    EXPECT_TRUE(left != right);
}

TEST(Ruvia, model_equality_compares_nested_arrays_and_recursive_boxed_values) {
    constexpr auto input = R"({"value":1,"items":[{"text":"a"},{"text":"b"}],"children":[{"value":2,"children":[{"value":3}]}]})";
    {
        auto first = ruvia::from_json<comparable_tree>(input);
        auto second = ruvia::from_json<comparable_tree>(input);
        EXPECT_TRUE(first == second);
        second.ensure<"items">()[0].set<"text">("b");
        EXPECT_TRUE(first != second);
        second.ensure<"items">()[0].set<"text">("a");
        EXPECT_TRUE(first == second);
        second.ensure<"children">().front().ensure<"children">().front().set<"value">(4);
        EXPECT_TRUE(first != second);
    }
}

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
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<optional_default>(input); }));
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    auto null = ruvia::from_json<nullable_default>(R"({"value":null})");
    EXPECT_TRUE(null.is_null<"value">());
    EXPECT_EQ(evaluations, std::size_t{0});
    const auto valid_form = ruvia::from_form<optional_default>("value=9");
    EXPECT_EQ(valid_form.get<"value">()->value_, 9);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<optional_default>("value=wrong"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<optional_default>("value=8&value=9"); }));
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
    const std::string prefix("{\"name\":\"" + std::string(128, 'n') + "\"");
    {
        auto supplied = ruvia::from_json<failing_default_model>(std::string(prefix) + ",\"value\":9}");
        EXPECT_EQ(supplied.get<"value">()->value_, 9);
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    for (std::size_t index = 0; index < 32; ++index) {
        bool failed = false;
        try {
            (void)ruvia::from_json<failing_default_model>(std::string(prefix) + "}");
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "default evaluation failed";
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(evaluations, index + 1);
    }
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
    {
        owned_default_model retained;
        {
            auto parsed_value = ruvia::from_json<owned_default_model>("{}");
            EXPECT_EQ(evaluations, std::size_t{1});
            retained = std::move(parsed_value);
        }
        EXPECT_EQ(evaluations, std::size_t{1});
        EXPECT_EQ(retained.get<"value">()->view(), std::string_view(std::string(128, 'd')));
        EXPECT_TRUE(!retained.is_present<"value">());
        retained.reset<"value">();
    }
    EXPECT_EQ(evaluations, std::size_t{1});
}

namespace {

RUVIA_MODEL(remark_request, RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));

RUVIA_MODEL(remark_with_default_request,
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));

RUVIA_MODEL(required_remark_request, RUVIA_REQUIRED_FIELD(remark, ruvia::string));

RUVIA_MODEL(json_bag_request, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<ruvia::json_value>));

RUVIA_MODEL(json_bag_response, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object));

RUVIA_MODEL(emission_options,
    RUVIA_OPTIONAL_FIELD_NAME("p\"lain", plain, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(omitted, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(both, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL));

}  // namespace

TEST(Ruvia, nullable_optional_string_accepts_json_null) {
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":null})");
    EXPECT_TRUE(!parsed_value.get<"remark">().has_value());
    EXPECT_TRUE(parsed_value.is_present<"remark">());
    EXPECT_TRUE(parsed_value.is_null<"remark">());
}

TEST(Ruvia, nullable_optional_string_still_accepts_text) {
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":"note"})");
    if (!parsed_value.get<"remark">()) {
        return;
    }
    EXPECT_EQ(parsed_value.get<"remark">()->view(), std::string_view("note"));
}

TEST(Ruvia, nullable_null_does_not_apply_default) {
    const auto parsed_value = ruvia::from_json<remark_with_default_request>(
        R"({"remark":null})");
    EXPECT_TRUE(!parsed_value.get<"remark">().has_value());
    EXPECT_TRUE(parsed_value.is_present<"remark">());
    EXPECT_TRUE(parsed_value.is_null<"remark">());

    const auto missing =
        ruvia::from_json<remark_with_default_request>("{}");
    if (!missing.get<"remark">()) {
        return;
    }
    EXPECT_EQ(missing.get<"remark">()->view(), std::string_view("fallback"));
}

TEST(Ruvia, required_string_rejects_json_null) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<required_remark_request>(R"({"remark":null})");
    }));
}

TEST(Ruvia, json_value_model_fields_accept_any_json_token) {
    const auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,{"k":null}],"object":{"a":true},"items":[null,"x",{"z":2}]})");
    if (!parsed_value.get<"payload">() || !parsed_value.get<"object">() ||
        !parsed_value.get<"items">()) {
        return;
    }
    EXPECT_TRUE(parsed_value.get<"payload">()->is_array());
    EXPECT_EQ(parsed_value.get<"payload">()->view(), std::string_view(R"([1,{"k":null}])"));
    EXPECT_EQ(parsed_value.get<"object">()->view(), std::string_view(R"({"a":true})"));
    const auto& items = *parsed_value.get<"items">();
    EXPECT_EQ(items.size(), std::size_t{3});
    EXPECT_TRUE(items[0].is_null());
    EXPECT_TRUE(items[1].is_string());
}

TEST(Ruvia, json_object_model_field_rejects_non_objects) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<json_bag_request>(R"({"object":[1]})");
    }));

    const auto null_object =
        ruvia::from_json<json_bag_request>(R"({"object":null})");
    EXPECT_TRUE(!null_object.get<"object">().has_value());
    EXPECT_TRUE(null_object.is_present<"object">());
    EXPECT_TRUE(null_object.is_null<"object">());
}

TEST(Ruvia, json_value_model_fields_own_tokens_for_from_json) {
    auto parsed_value = ruvia::from_json<json_bag_request>(
        std::string(R"({"payload":{"keep":true}})"));
    if (!parsed_value.get<"payload">()) {
        return;
    }
    EXPECT_TRUE(parsed_value.get<"payload">()->is_object());
    EXPECT_EQ(parsed_value.get<"payload">()->view(), std::string_view(R"({"keep":true})"));
}

TEST(Ruvia, json_value_response_fields_write_raw_tokens) {
    auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,2],"object":{"a":1}})");
    if (!parsed_value.get<"payload">() || !parsed_value.get<"object">()) {
        return;
    }

    json_bag_response response;
    response.set<"payload">(*parsed_value.get<"payload">());
    response.set<"object">(std::move(parsed_value.ensure<"object">()));
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"payload":[1,2],"object":{"a":1}})"));
}

TEST(Ruvia, model_json_emission_options_write_expected_output_for_all_states) {
    // Each pair of bits selects missing, explicit null, empty, or text. The
    // Cartesian product also covers every comma position after omitted fields.
    constexpr std::string_view names[] = {R"("p\"lain")", R"("omitted")", R"("emitted")", R"("both")"};
    for (unsigned combination = 0; combination != 256; ++combination) {
        emission_options value;
        const auto assign = [&]<ruvia::fixed_string field>(unsigned state_value) {
            if (state_value == 1) {
                value.set<field>(nullptr);
            } else if (state_value == 2) {
                value.set<field>("");
            } else if (state_value == 3) {
                value.set<field>("\"\n");
            }
        };
        assign.template operator()<"plain">(combination & 3);
        assign.template operator()<"omitted">((combination >> 2) & 3);
        assign.template operator()<"emitted">((combination >> 4) & 3);
        assign.template operator()<"both">((combination >> 6) & 3);

        std::string expected("{");
        for (unsigned index = 0; index != 4; ++index) {
            const auto state_value = (combination >> (index * 2)) & 3;
            if ((state_value == 0 && index < 2) || (state_value == 2 && (index == 1 || index == 3))) {
                continue;
            }
            if (expected.size() != 1) {
                expected.push_back(',');
            }
            expected.append(names[index]);
            expected.push_back(':');
            expected.append(state_value < 2 ? "null" : state_value == 2 ? R"("")"
                                                                        : R"("\"\n")");
        }
        expected.push_back('}');
        const auto output = ruvia::to_json(value);
        EXPECT_EQ(std::string_view(output), std::string_view(expected));
    }
}

namespace {

RUVIA_MODEL(required_value, RUVIA_REQUIRED_FIELD(value, ruvia::string));
RUVIA_MODEL(optional_value, RUVIA_OPTIONAL_FIELD(value, ruvia::string));
RUVIA_MODEL(required_nullable_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(optional_nullable_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(nullable_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(invalid_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::uint32, RUVIA_DEFAULT(3), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(named_value,
    RUVIA_OPTIONAL_FIELD_NAME("wire_value", value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));
RUVIA_MODEL(patch_value,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(nested_values,
    RUVIA_REQUIRED_FIELD(children, ruvia::array<nullable_default_value>));
RUVIA_MODEL(nullable_kinds,
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(number, ruvia::uint32, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(child, required_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::string>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(boxes, ruvia::boxed_array<required_value>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object, RUVIA_NULLABLE));
RUVIA_MODEL(dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value));
RUVIA_MODEL(nullable_dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(required_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value));
RUVIA_MODEL(required_nullable_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(dynamic_object,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_object));
RUVIA_MODEL(owned_values,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string, RUVIA_DEFAULT("a default string long enough to allocate storage")));
RUVIA_MODEL(nullable_response,
    RUVIA_REQUIRED_FIELD(required, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(dynamic, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(form_unrepresentable_values,
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(data, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(child, required_value),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string));

// Models spell value presence only as has_value; entity is_set means "assigned,
// null included" and must not appear on models with a different meaning.
template <typename model_type>
concept model_offers_has_value = requires(const model_type& model) { model.template has_value<"optional">(); };
template <typename model_type>
concept model_offers_is_set = requires(const model_type& model) { model.template is_set<"optional">(); };
static_assert(model_offers_has_value<nullable_response>);
static_assert(!model_offers_is_set<nullable_response>);

template <typename model_type>
void check_presence_matrix(bool required, bool nullable_value) {
    for (auto input : {R"({"value":42})", R"({"value":false})", R"({"value":[]})", R"({"value":{}})"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    for (auto input : {R"({"value":null,"value":"note"})",
             R"({"value":"note","value":null})", R"({"value":null,"value":null})",
             R"({"value":42,"value":"note"})", R"({"value":"note","va\u006cue":"again"})"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    EXPECT_EQ(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>("{}"); }), required);
    EXPECT_EQ(
        ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":null})"); }),
        !nullable_value);
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":"note"})"); }));
}

}  // namespace

TEST(Ruvia, model_field_presence_and_nullability_are_independent) {
    check_presence_matrix<required_value>(true, false);
    check_presence_matrix<optional_value>(false, false);
    check_presence_matrix<required_nullable_value>(true, true);
    check_presence_matrix<optional_nullable_value>(false, true);
    auto null = ruvia::from_json<required_nullable_value>(R"({"value":null})");
    auto text = ruvia::from_json<required_nullable_value>(R"({"value":"note"})");
    EXPECT_TRUE(!null.get<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "note");
}

TEST(Ruvia, model_patch_public_states_distinguish_omission_clear_and_assignment) {
    std::optional<std::string> remark(std::in_place, "original");
    auto apply = [&](std::string_view json) {
        const auto patch = ruvia::from_json<patch_value>(json);
        if (!patch.is_present<"remark">()) {
            return;
        }
        if (patch.is_null<"remark">()) {
            remark.reset();
        } else {
            remark.emplace(patch.get<"remark">()->view());
        }
    };
    apply("{}");
    EXPECT_TRUE(remark == "original");
    apply(R"({"remark":null})");
    EXPECT_TRUE(!remark);
    apply(R"({"remark":"replacement"})");
    EXPECT_TRUE(remark == "replacement");
    apply(R"({"remark":""})");
    EXPECT_TRUE(remark && remark->empty());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<patch_value>(R"({"enabled":null})"); }));
    const auto disabled = ruvia::from_json<patch_value>(R"({"enabled":false})");
    EXPECT_TRUE(disabled.is_present<"enabled">() && !disabled.is_null<"enabled">());
    EXPECT_TRUE(!bool(*disabled.get<"enabled">()));
}

TEST(Ruvia, model_defaults_only_fill_optional_missing_values) {
    auto missing = ruvia::from_json<nullable_default_value>("{}");
    EXPECT_TRUE(missing.get<"value">()->view() == "fallback");
    EXPECT_TRUE(!missing.is_present<"value">());
    auto form = ruvia::from_form<default_value>("");
    EXPECT_TRUE(!form.is_present<"value">() && form.get<"value">()->view() == "fallback");
    bool default_rule_rejected = false;
    try {
        (void)ruvia::from_form<invalid_default_value>("");
    } catch (const ruvia::http_error&) {
        default_rule_rejected = true;
    }
    EXPECT_TRUE(default_rule_rejected);
}

TEST(Ruvia, model_input_presence_survives_application_mutation_and_moves) {
    auto absent = ruvia::from_json<nullable_default_value>("{}");
    auto present = ruvia::from_json<nullable_default_value>(R"({"value":null})");
    absent.set<"value">("application value");
    EXPECT_TRUE(!absent.is_present<"value">());
    present.ensure<"value">().assign_owned(std::string_view("replacement"));
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.reset<"value">();
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.set<"value">(nullptr);
    nullable_default_value moved(std::move(present));
    EXPECT_TRUE(moved.is_present<"value">() && moved.is_null<"value">());
    EXPECT_TRUE(!present.is_present<"value">() && !present.is_null<"value">());
    absent = std::move(moved);
    EXPECT_TRUE(absent.is_present<"value">() && absent.is_null<"value">());
    EXPECT_TRUE(!moved.is_present<"value">());
}

TEST(Ruvia, model_named_form_and_nested_fields_keep_input_presence) {
    auto named = ruvia::from_json<named_value>(R"({"wire_value":null})");
    EXPECT_TRUE(named.is_present<"value">() && named.is_null<"value">());
    auto text = ruvia::from_form<named_value>("wire_value=null");
    EXPECT_TRUE(text.is_present<"value">() && !text.is_null<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "null");
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<named_value>("wire_value=a&wire_value=b"); }));
    auto nested = ruvia::from_json<nested_values>(R"({"children":[{},{"value":null},{"value":"note"}]})");
    const auto& children = nested.get<"children">();
    EXPECT_TRUE(!children[0].is_present<"value">() && children[0].get<"value">()->view() == "fallback");
    EXPECT_TRUE(children[1].is_present<"value">() && children[1].is_null<"value">());
    EXPECT_TRUE(children[2].is_present<"value">() && !children[2].is_null<"value">());
}

TEST(Ruvia, model_form_rejects_values_for_fields_without_form_representation) {
    auto absent = ruvia::from_form<form_unrepresentable_values>("note=a&unknown=b");
    EXPECT_TRUE(absent.is_present<"note">() && !absent.is_present<"tags">());
    for (const auto body : {"tags=a", "tags=a&tags=b", "data=a", "child=a", "payload=1", "object=%7B%7D"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<form_unrepresentable_values>(body); }));
    }
}

TEST(Ruvia, model_nullable_applies_to_scalar_container_nested_and_dynamic_types) {
    auto parsed_value = ruvia::from_json<nullable_kinds>(
        R"({"flag":null,"number":null,"child":null,"items":null,"boxes":null,"payload":null,"object":null})");
    EXPECT_TRUE(parsed_value.is_present<"flag">() && parsed_value.is_null<"flag">() && !parsed_value.get<"flag">());
    EXPECT_TRUE(parsed_value.is_null<"number">() && !parsed_value.get<"number">());
    EXPECT_TRUE(parsed_value.is_null<"child">() && !parsed_value.get<"child">());
    EXPECT_TRUE(parsed_value.is_null<"items">() && !parsed_value.get<"items">());
    EXPECT_TRUE(parsed_value.is_null<"boxes">() && !parsed_value.get<"boxes">());
    EXPECT_TRUE(parsed_value.is_null<"payload">() && !parsed_value.get<"payload">());
    EXPECT_TRUE(parsed_value.is_null<"object">() && !parsed_value.get<"object">());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":[]})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":{"nested":null}})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":[null,false,1]})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":{"nested":null}})"); }));
}

TEST(Ruvia, model_nonnullable_dynamic_assignment_rejects_null_without_changing_value) {
    auto model = ruvia::from_json<dynamic_value>(R"({"value":1})");
    auto null = ruvia::json_value::parse("null");
    bool rejected = false;
    try {
        model.set<"value">(std::move(null));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(model.is_present<"value">());
    EXPECT_TRUE(!model.is_null<"value">());
    EXPECT_EQ(model.get<"value">()->view(), std::string_view("1"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    EXPECT_TRUE(null.is_null());
}

TEST(Ruvia, model_nonnullable_dynamic_setter_preserves_absence_on_null_rejection) {
    dynamic_value model;
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(!model.get<"value">());
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

    model.set<"value">(ruvia::json_value::parse(R"({"nested":null})"));
    EXPECT_EQ(model.get<"value">()->view(), std::string_view(R"({"nested":null})"));
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":{"nested":null}})"));
}

TEST(Ruvia, model_nullable_dynamic_setter_keeps_value_null_and_input_presence_consistent) {
    for (const auto input : {"{}", R"({"value":null})"}) {
        auto model = ruvia::from_json<nullable_dynamic_value>(input);
        const bool present = model.is_present<"value">();
        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(nullptr);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));

        model.set<"value">(ruvia::json_value::parse("[null,false,1]"));
        EXPECT_EQ(model.get<"value">()->view(), std::string_view("[null,false,1]"));
        EXPECT_TRUE(!model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":[null,false,1]})"));

        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(ruvia::json_value::parse("true"));
        const auto null = ruvia::json_value::parse("null");
        model.set<"value">(null);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));
    }
}

TEST(Ruvia, model_required_dynamic_setters_preserve_required_and_nullable_contracts) {
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>("{}"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_nullable_dynamic_value>("{}"); }));
    auto model = ruvia::from_json<required_dynamic_value>(R"({"value":1})");
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(model.get<"value">().view(), std::string_view("1"));
    EXPECT_TRUE(model.is_present<"value">() && !model.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    model.set<"value">(ruvia::json_value::parse("false"));
    EXPECT_EQ(model.get<"value">().view(), std::string_view("false"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":false})"));

    required_nullable_dynamic_value response;
    ruvia::validator missing;
    ruvia::validate_model(response, missing);
    EXPECT_TRUE(!missing.ok());
    response.set<"value">(nullptr);
    EXPECT_TRUE(!response.get<"value">() && response.is_null<"value">());
    EXPECT_TRUE(!response.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"value":null})"));
    ruvia::validator valid;
    ruvia::validate_model(response, valid);
    EXPECT_TRUE(valid.ok());
    auto parsed = ruvia::from_json<required_nullable_dynamic_value>(R"({"value":null})");
    parsed.set<"value">(ruvia::json_value::parse("42"));
    EXPECT_EQ(parsed.get<"value">()->view(), std::string_view("42"));
    EXPECT_TRUE(parsed.is_present<"value">() && !parsed.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":42})"));
    parsed.set<"value">(ruvia::json_value::parse("null"));
    EXPECT_TRUE(!parsed.get<"value">() && parsed.is_null<"value">() && parsed.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":null})"));
}

TEST(Ruvia, model_nullable_response_emits_explicit_null_without_emitting_absence) {
    nullable_response response;
    response.set<"required">(nullptr);
    EXPECT_TRUE(response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"required":null})"));
    response.set<"optional">(nullptr);
    auto token = ruvia::json_value::parse("null");
    response.set<"dynamic">(std::move(token));
    EXPECT_TRUE(response.is_null<"dynamic">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":null,"optional":null,"dynamic":null})"));
    response.reset<"optional">();
    response.set<"required">("note");
    EXPECT_TRUE(!response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":"note","dynamic":null})"));
    response.set_null<"optional">().reset<"optional">().set<"optional">("val");
    EXPECT_TRUE(response.has_value<"optional">());
    response.set_null<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && response.is_null<"optional">());
    response.reset<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && !response.is_null<"optional">());
}

TEST(Ruvia, model_dynamic_owned_tokens_and_presence_survive_rebinding_and_reclaim_memory) {
    const std::string text(256, 'x');
    const std::string body(std::string("{\"value\":{\"text\":\"") + text + "\"},\"object\":{\"text\":\"" + text + "\"}}");
    {
        owned_values retained;
        {
            auto input = body;
            auto parsed_value = ruvia::from_json<owned_values>(input);
            input.assign(input.size(), '?');
            owned_values moved(std::move(parsed_value));
            EXPECT_TRUE(moved.is_present<"value">() && !moved.is_present<"note">());
            retained = std::move(moved);
        }
        EXPECT_TRUE(retained.is_present<"value">() && retained.is_present<"object">());
        EXPECT_TRUE(!retained.is_present<"note">() && retained.get<"note">().has_value());
        EXPECT_TRUE(retained.get<"value">()->get<ruvia::string>("text").value()->view() == text);
        for (int index = 0; index < 32; ++index) {
            {
                auto parsed_value = ruvia::from_json<owned_values>(body);
                EXPECT_TRUE(parsed_value.is_present<"value">());
            }
            for (const auto& invalid : {std::string(std::string_view(body).substr(0, body.size() - 1)) + ",\"value\":null}",
                     std::string(std::string_view(body).substr(0, body.size() - 1)) + ",\"note\":42}", std::string(std::string_view(body).substr(0, body.size() - 1))}) {
                EXPECT_TRUE(ruvia::test::throws_on(
                    [&] { (void)ruvia::from_json<owned_values>(invalid); }));
            }
            EXPECT_TRUE(retained.get<"object">()->get<ruvia::string>("text").value()->view() == text);
        }
    }
}

namespace {

RUVIA_MODEL(resource_child,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));
RUVIA_MODEL(resource_parent,
    RUVIA_REQUIRED_FIELD(title, ruvia::string),
    RUVIA_OPTIONAL_FIELD(child, resource_child),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_child>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<resource_child>));
RUVIA_MODEL(resource_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<resource_node>));
RUVIA_MODEL(resource_dense_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_dense_node>));
RUVIA_MODEL(resource_text_sequences,
    RUVIA_OPTIONAL_FIELD(dense, ruvia::array<ruvia::boxed_array<ruvia::string>>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<ruvia::string>));

}  // namespace

TEST(Ruvia, model_assignment_recursively_outlives_source_owner) {
    const std::string text(160, 'a');
    resource_parent target;
    {
        target.set<"title">(ruvia::string(text));
        resource_child source_value;
        source_value.set<"name">(text);
        source_value.ensure<"tags">().emplace_back(text);
        target.set<"child">(std::move(source_value));
        resource_child other;
        other.set<"name">(text);
        other.ensure<"tags">().emplace_back(text);
        // Public insertion accepts a const model and owns it recursively.
        const auto& borrowed = other;
        target.ensure<"children">().emplace_back(borrowed);
        target.ensure<"boxed">().emplace_back(std::move(other));
    }
    EXPECT_EQ(target.get<"title">().view(), std::string_view(text));
    const auto verify = [&](const resource_child& child_value) {
        EXPECT_EQ(child_value.get<"name">().view(), std::string_view(text));
        EXPECT_EQ(child_value.get<"tags">()->front().view(), std::string_view(text));
    };
    verify(*target.get<"child">());
    verify(target.get<"children">()->front());
    verify(target.get<"boxed">()->front());
}

TEST(Ruvia, model_array_construction_and_mutable_assignment_keep_values) {
    const std::string text(160, 'b');
    ruvia::array<ruvia::string> strings;
    strings.emplace_back(text);
    strings.emplace_back().assign_owned(text);
    ruvia::array<resource_child> children;
    children.emplace_back().set<"name">(text);
    ruvia::boxed_array<ruvia::string> boxed;
    boxed.emplace_back(text);
    {
        strings.front() = ruvia::string(text);
        resource_child replacement;
        replacement.set<"name">(text);
        children.front() = std::move(replacement);
        boxed.front() = ruvia::string(text);
    }
    for (const auto& value : strings) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
    EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));
    EXPECT_EQ(boxed.front().view(), std::string_view(text));
}

TEST(Ruvia, model_array_resize_accepts_its_own_fill_element) {
    const std::string text(160, 'd');
    ruvia::array<ruvia::string> values;
    values.emplace_back(text);
    const auto count = values.capacity() + 8;
    values.resize(count, values.front());
    EXPECT_EQ(values.size(), count);
    for (const auto& value : values) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
}

TEST(Ruvia, model_recursive_assignment_can_promote_an_owned_child) {
    const std::string text(160, 'e');
    resource_node root;
    root.set<"name">("parent");
    auto& child_value = root.ensure<"children">().emplace_back();
    child_value.set<"name">(text);
    child_value.ensure<"children">().emplace_back().set<"name">("grandchild");
    root = std::move(child_value);
    EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("grandchild"));

    auto& children = root.ensure<"children">();
    children.front().ensure<"children">().emplace_back().set<"name">(text);
    children = std::move(children.front().ensure<"children">());
    EXPECT_EQ(children.size(), std::size_t{1});
    EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));

    resource_dense_node dense;
    auto& dense_children = dense.ensure<"children">();
    dense_children.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
    dense_children = std::move(dense_children.front().ensure<"children">());
    EXPECT_EQ(dense_children.size(), std::size_t{1});
    EXPECT_EQ(dense_children.front().get<"name">().view(), std::string_view(text));
}

TEST(Ruvia, model_moves_reject_a_source_that_owns_the_destination) {
    const std::string text(160, 'f');
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    resource_node root;
    root.set<"name">(text);
    auto& child_value = root.ensure<"children">().emplace_back();
    child_value.set<"name">("child");
    auto& grandchildren = child_value.ensure<"children">();
    grandchildren.emplace_back().set<"name">("grandchild");

    EXPECT_TRUE(rejects([&] { child_value = std::move(root); }));
    EXPECT_TRUE(rejects([&] { grandchildren = std::move(root.ensure<"children">()); }));
    EXPECT_TRUE(rejects([&] { grandchildren.push_back(std::move(root)); }));
    EXPECT_TRUE(rejects([&] { root.ensure<"children">().emplace_back(std::move(root)); }));
    EXPECT_TRUE(rejects([&] { child_value.set<"children">(std::move(root.ensure<"children">())); }));

    EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(root.get<"children">()->size(), std::size_t{1});
    EXPECT_TRUE(&root.get<"children">()->front() == &child_value);
    EXPECT_EQ(child_value.get<"name">().view(), std::string_view("child"));
    EXPECT_EQ(grandchildren.size(), std::size_t{1});
    EXPECT_EQ(grandchildren.front().get<"name">().view(), std::string_view("grandchild"));

    // A field reassigned from its own value does not own its destination.
    root.set<"children">(std::move(root.ensure<"children">()));
    EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("child"));

    resource_dense_node dense;
    dense.set<"name">(text);
    auto& dense_children = dense.ensure<"children">();
    auto& dense_child = dense_children.emplace_back();
    dense_child.set<"name">("child");
    auto& dense_grandchildren = dense_child.ensure<"children">();

    EXPECT_TRUE(rejects([&] { dense_child = std::move(dense); }));
    EXPECT_TRUE(rejects([&] { dense_grandchildren = std::move(dense_children); }));
    EXPECT_TRUE(rejects([&] { dense_grandchildren.emplace_back(std::move(dense)); }));
    EXPECT_TRUE(rejects([&] { dense_children.emplace_back(std::move(dense)); }));

    EXPECT_EQ(dense.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(dense_children.size(), std::size_t{1});
    EXPECT_EQ(dense_child.get<"name">().view(), std::string_view("child"));
    EXPECT_TRUE(dense_grandchildren.empty());
}

TEST(Ruvia, model_self_field_and_aliased_element_moves_preserve_values) {
    const std::string first(160, 'a');
    const std::string second(160, 'b');
    resource_text_sequences value;
    value.ensure<"dense">().emplace_back().emplace_back(first);
    value.ensure<"dense">().emplace_back().emplace_back(second);
    value.set<"dense">(std::move(value.ensure<"dense">()));
    ASSERT_EQ(value.get<"dense">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"dense">())[0].front().view(), std::string_view(first));
    EXPECT_EQ((*value.get<"dense">())[1].front().view(), std::string_view(second));

    value.ensure<"boxed">().emplace_back(first);
    value.ensure<"boxed">().emplace_back(second);
    value.set<"boxed">(std::move(value.ensure<"boxed">()));
    ASSERT_EQ(value.get<"boxed">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"boxed">())[0].view(), std::string_view(first));
    EXPECT_EQ((*value.get<"boxed">())[1].view(), std::string_view(second));

    ruvia::array<ruvia::string> values;
    values.emplace_back(first);
    while (values.size() < values.capacity()) {
        values.emplace_back(second);
    }
    const char* const original_text = values.front().data();
    values.push_back(std::move(values.front()));
    EXPECT_EQ(values.back().view(), std::string_view(first));
    EXPECT_EQ(values.back().data(), original_text);
    EXPECT_TRUE(values.front().empty());
}

TEST(Ruvia, model_scalar_array_moves_transfer_storage) {
    const auto check = []<typename sequence_type>() {
        sequence_type source;
        sequence_type target;
        for (int index = 0; index < 64; ++index) {
            source.push_back(ruvia::int32(index));
        }
        target.push_back(ruvia::int32(-1));
        target = std::move(source);
        EXPECT_TRUE(source.empty());
        EXPECT_EQ(target.size(), std::size_t{64});
        for (std::size_t index = 0; index < target.size(); ++index) {
            EXPECT_EQ(target[index].value_, static_cast<int>(index));
        }
    };
    check.template operator()<ruvia::array<ruvia::int32>>();
    check.template operator()<ruvia::boxed_array<ruvia::int32>>();
}

static_assert(!std::is_copy_constructible_v<resource_parent>);
static_assert(!std::is_constructible_v<resource_parent, resource_child&>);
