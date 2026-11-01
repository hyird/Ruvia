#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>

#include "ruvia/http/http_number_format.h"

#include "test_harness.h"

RUVIA_TEST(number_format_decimal_width_and_pmr_output) {
    RUVIA_CHECK_EQ(ruvia::http_unsigned_decimal_size(0), std::size_t{1});
    RUVIA_CHECK_EQ(ruvia::http_unsigned_decimal_size(999), std::size_t{3});
    RUVIA_CHECK_EQ(ruvia::http_unsigned_decimal_size((std::numeric_limits<std::uint64_t>::max)()),
        std::size_t{20});

    std::pmr::string output;
    ruvia::http_append_signed(output, std::int64_t{-42});
    ruvia::http_append_unsigned(output, std::uint64_t{123});
    RUVIA_CHECK_EQ(output, "-42123");
}

RUVIA_TEST(number_format_signed_extremes) {
    std::pmr::string output;
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::min)());
    RUVIA_CHECK_EQ(output, "-9223372036854775808");
    output.clear();
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::max)());
    RUVIA_CHECK_EQ(output, "9223372036854775807");
}
