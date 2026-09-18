#include "ruvia/core/Integer.h"

#include <cstdint>
#include <limits>
#include <string_view>

#include "test_harness.h"

RUVIA_TEST(integer_decimal_limits) {
    RUVIA_CHECK_EQ(*ruvia::parseInteger<std::int64_t>("-9223372036854775808"), (std::numeric_limits<std::int64_t>::min)());
    RUVIA_CHECK_EQ(*ruvia::parseInteger<std::uint64_t>("18446744073709551615"), (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(*ruvia::parseInteger<int>("00042"), 42);
    RUVIA_CHECK_EQ(*ruvia::parseInteger<int>("-0"), 0);
    for (auto text : {"128", "-129"}) {
        RUVIA_CHECK_EQ(ruvia::parseInteger<std::int8_t>(text).error(), ruvia::IntegerParseError::kOutOfRange);
    }
    RUVIA_CHECK_EQ(ruvia::parseInteger<std::uint64_t>("18446744073709551616").error(), ruvia::IntegerParseError::kOutOfRange);
}

RUVIA_TEST(integer_requires_complete_decimal_input) {
    for (auto text : {"", " ", "+1", " 1", "1 ", "1a", "0x10", "-", "1.0", "999999999999999999999999x"}) {
        RUVIA_CHECK_EQ(ruvia::parseInteger<int>(text).error(), ruvia::IntegerParseError::kInvalidFormat);
    }
    RUVIA_CHECK_EQ(ruvia::parseInteger<unsigned>("-1").error(), ruvia::IntegerParseError::kInvalidFormat);
    RUVIA_CHECK_EQ(ruvia::parseInteger<int>(std::string_view("12\0x", 4)).error(), ruvia::IntegerParseError::kInvalidFormat);
    RUVIA_CHECK_EQ(*ruvia::parseInteger<int>("4").transform([](int value) { return value * 2; }), 8);
}
