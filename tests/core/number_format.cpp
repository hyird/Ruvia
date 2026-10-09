#include "ruvia/core/number_format.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>

#include "test_harness.h"

RUVIA_TEST(number_format_decimal_width_and_pmr_output) {
    RUVIA_CHECK_EQ(ruvia::unsigned_decimal_size(0), std::size_t{1});
    RUVIA_CHECK_EQ(ruvia::unsigned_decimal_size(999), std::size_t{3});
    RUVIA_CHECK_EQ(ruvia::unsigned_decimal_size((std::numeric_limits<std::uint64_t>::max)()),
        std::size_t{20});

    std::pmr::string output;
    ruvia::append_formatted_number(output, std::int64_t{-42}, "format failure");
    ruvia::append_formatted_number(output, std::uint64_t{123}, "format failure");
    RUVIA_CHECK_EQ(output, "-42123");
    ruvia::append_formatted_finite_number(output, 1.5, "nonfinite", "format failure");
    RUVIA_CHECK_EQ(output, "-421231.5");
}

RUVIA_TEST(number_format_rejects_nonfinite_without_modifying_output) {
    std::pmr::string output("prefix");
    bool rejected = false;
    try {
        ruvia::append_formatted_finite_number(output, std::numeric_limits<double>::infinity(),
            "nonfinite", "format failure");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(output, "prefix");
}
