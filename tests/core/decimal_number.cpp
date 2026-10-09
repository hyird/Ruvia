#include "decimal_number.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <variant>

#include "test_harness.h"

using ruvia::detail::decimal_parse_error;
using ruvia::detail::parse_decimal_number;

RUVIA_TEST(decimal_number_converts_directly_to_the_target_float_type) {
    const auto rounded = parse_decimal_number<float>("1.000000059604644775390626");
    RUVIA_CHECK((rounded.index() == 0));
    if ((rounded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(rounded), std::nextafter(1.0f, 2.0f));
    }
    const auto smallest = parse_decimal_number<float>("1.401298464324817e-45");
    RUVIA_CHECK((smallest.index() == 0));
    if ((smallest.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(smallest), std::numeric_limits<float>::denorm_min());
    }
    for (const auto text : {"1e-46", "1e39"}) {
        const auto parsed_value = parse_decimal_number<float>(text);
        RUVIA_CHECK((parsed_value.index() != 0));
        if ((parsed_value.index() != 0)) {
            RUVIA_CHECK_EQ(std::get<1>(parsed_value), decimal_parse_error::out_of_range);
        }
    }
    RUVIA_CHECK(std::signbit(std::get<0>(parse_decimal_number<float>("-0e99999"))));
    RUVIA_CHECK_EQ(std::get<0>(parse_decimal_number<long double>("1.25")), 1.25L);
}

RUVIA_TEST(decimal_number_handles_representable_extremes_and_long_significands) {
    const struct {
        std::string_view text_;
        double value_;
    } cases[] = {
        {"4.9406564584124654e-324", std::numeric_limits<double>::denorm_min()},
        {"-4.9406564584124654e-324", -std::numeric_limits<double>::denorm_min()},
        {"2.2250738585072014e-308", (std::numeric_limits<double>::min)()},
        {"1.7976931348623157e308", (std::numeric_limits<double>::max)()},
        {"1.00000000000000011102230246251565404236316680908203124", 1.0},
        {"1.00000000000000011102230246251565404236316680908203126", std::nextafter(1.0, 2.0)},
    };
    for (const auto& test : cases) {
        const auto parsed_value = parse_decimal_number(test.text_);
        RUVIA_CHECK((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(parsed_value), test.value_);
        }
    }
}

RUVIA_TEST(decimal_number_returns_values_for_decimal_and_exponent_forms) {
    for (const auto text : {"125", "00125", "1.25e2", "1250e-1", "125E+0"}) {
        const auto parsed_value = parse_decimal_number(text);
        RUVIA_CHECK((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(parsed_value), 125.0);
        }
    }
    RUVIA_CHECK_EQ(std::get<0>(parse_decimal_number("-.5")), -0.5);
    RUVIA_CHECK_EQ(std::get<0>(parse_decimal_number("12.5")), 12.5);
}

RUVIA_TEST(decimal_number_preserves_zero_sign_and_explicit_nonfinite_values) {
    for (const auto text : {"-0", "-0.0", "-0e99999"}) {
        const auto parsed_value = parse_decimal_number(text);
        RUVIA_CHECK((parsed_value.index() == 0));
        if ((parsed_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(parsed_value), 0.0);
            RUVIA_CHECK(std::signbit(std::get<0>(parsed_value)));
        }
    }
    RUVIA_CHECK(!std::signbit(std::get<0>(parse_decimal_number("0"))));
    RUVIA_CHECK(std::isinf(std::get<0>(parse_decimal_number("infinity"))));
    RUVIA_CHECK(std::signbit(std::get<0>(parse_decimal_number("-inf"))));
    RUVIA_CHECK(std::isnan(std::get<0>(parse_decimal_number("nan"))));
}

RUVIA_TEST(decimal_number_distinguishes_invalid_format_from_range_errors) {
    for (const auto text : {"", "-", "+1", " 1", "1 ", "1.", "1e", "1e+", "1.2.3", "1e99999x",
             "NAN", "INF", "+inf", "nan(payload)", "0x1p2"}) {
        const auto parsed_value = parse_decimal_number(text);
        RUVIA_CHECK((parsed_value.index() != 0));
        if ((parsed_value.index() != 0)) {
            RUVIA_CHECK_EQ(std::get<1>(parsed_value), decimal_parse_error::invalid_format);
        }
    }
    const auto embedded_null = parse_decimal_number(std::string_view("12\0x", 4));
    RUVIA_CHECK((embedded_null.index() != 0));
    for (const auto text : {"1e99999", "-1e99999", "1e-99999", "-1e-99999"}) {
        const auto parsed_value = parse_decimal_number(text);
        RUVIA_CHECK((parsed_value.index() != 0));
        if ((parsed_value.index() != 0)) {
            RUVIA_CHECK_EQ(std::get<1>(parsed_value), decimal_parse_error::out_of_range);
        }
    }
}
