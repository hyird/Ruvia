#include <cmath>
#include <limits>
#include <string_view>

#include "ruvia/core/detail/number/DecimalNumber.h"

#include "test_harness.h"

using ruvia::detail::DecimalParseError;
using ruvia::detail::parseDecimalNumber;

RUVIA_TEST(decimal_number_converts_directly_to_the_target_float_type) {
    const auto rounded = parseDecimalNumber<float>("1.000000059604644775390626");
    RUVIA_CHECK(rounded.has_value());
    if (rounded) {
        RUVIA_CHECK_EQ(*rounded, std::nextafter(1.0f, 2.0f));
    }
    const auto smallest = parseDecimalNumber<float>("1.401298464324817e-45");
    RUVIA_CHECK(smallest.has_value());
    if (smallest) {
        RUVIA_CHECK_EQ(*smallest, std::numeric_limits<float>::denorm_min());
    }
    for (const auto text : {"1e-46", "1e39"}) {
        const auto parsed = parseDecimalNumber<float>(text);
        RUVIA_CHECK(!parsed);
        if (!parsed) {
            RUVIA_CHECK_EQ(parsed.error(), DecimalParseError::kOutOfRange);
        }
    }
    RUVIA_CHECK(std::signbit(parseDecimalNumber<float>("-0e99999").value()));
    RUVIA_CHECK_EQ(parseDecimalNumber<long double>("1.25").value(), 1.25L);
}

RUVIA_TEST(decimal_number_handles_representable_extremes_and_long_significands) {
    const struct {
        std::string_view text;
        double value;
    } cases[] = {
        {"4.9406564584124654e-324", std::numeric_limits<double>::denorm_min()},
        {"-4.9406564584124654e-324", -std::numeric_limits<double>::denorm_min()},
        {"2.2250738585072014e-308", (std::numeric_limits<double>::min)()},
        {"1.7976931348623157e308", (std::numeric_limits<double>::max)()},
        {"1.00000000000000011102230246251565404236316680908203124", 1.0},
        {"1.00000000000000011102230246251565404236316680908203126", std::nextafter(1.0, 2.0)},
    };
    for (const auto& test : cases) {
        const auto parsed = parseDecimalNumber(test.text);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(*parsed, test.value);
        }
    }
}

RUVIA_TEST(decimal_number_returns_values_for_decimal_and_exponent_forms) {
    for (const auto text : {"125", "00125", "1.25e2", "1250e-1", "125E+0"}) {
        const auto parsed = parseDecimalNumber(text);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(*parsed, 125.0);
        }
    }
    RUVIA_CHECK_EQ(parseDecimalNumber("-.5").value(), -0.5);
    RUVIA_CHECK_EQ(parseDecimalNumber("12.5").value(), 12.5);
}

RUVIA_TEST(decimal_number_preserves_zero_sign_and_explicit_nonfinite_values) {
    for (const auto text : {"-0", "-0.0", "-0e99999"}) {
        const auto parsed = parseDecimalNumber(text);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(*parsed, 0.0);
            RUVIA_CHECK(std::signbit(*parsed));
        }
    }
    RUVIA_CHECK(!std::signbit(parseDecimalNumber("0").value()));
    RUVIA_CHECK(std::isinf(parseDecimalNumber("infinity").value()));
    RUVIA_CHECK(std::signbit(parseDecimalNumber("-inf").value()));
    RUVIA_CHECK(std::isnan(parseDecimalNumber("nan").value()));
}

RUVIA_TEST(decimal_number_distinguishes_invalid_format_from_range_errors) {
    for (const auto text : {"", "-", "+1", " 1", "1 ", "1.", "1e", "1e+", "1.2.3", "1e99999x",
             "NAN", "INF", "+inf", "nan(payload)", "0x1p2"}) {
        const auto parsed = parseDecimalNumber(text);
        RUVIA_CHECK(!parsed);
        if (!parsed) {
            RUVIA_CHECK_EQ(parsed.error(), DecimalParseError::kInvalidFormat);
        }
    }
    const auto embeddedNull = parseDecimalNumber(std::string_view("12\0x", 4));
    RUVIA_CHECK(!embeddedNull);
    for (const auto text : {"1e99999", "-1e99999", "1e-99999", "-1e-99999"}) {
        const auto parsed = parseDecimalNumber(text);
        RUVIA_CHECK(!parsed);
        if (!parsed) {
            RUVIA_CHECK_EQ(parsed.error(), DecimalParseError::kOutOfRange);
        }
    }
}
