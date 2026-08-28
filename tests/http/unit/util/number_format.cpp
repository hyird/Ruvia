#include "test_harness.h"

#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>

#include "ruvia/http/detail/util/HttpNumberFormat.h"

// Writing numbers into HTTP field values, including the finite check a
// formatted double must pass.

RUVIA_TEST(number_unsigned_decimal_size) {
    using ruvia::detail::httpUnsignedDecimalSize;
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(0), std::size_t(1));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(9), std::size_t(1));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(10), std::size_t(2));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(99), std::size_t(2));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(100), std::size_t(3));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(UINT64_C(18446744073709551615)), std::size_t(20));
}

RUVIA_TEST(number_append_formatted) {
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::appendHttpFormattedNumber(out, 42, "err");
    ruvia::detail::appendHttpFormattedNumber(out, -7, "err");
    RUVIA_CHECK_EQ(std::string(out.c_str()), std::string("42-7"));
}

RUVIA_TEST(number_append_formatted_finite_rejects_non_finite) {
    // A finite double formats as usual.
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::appendHttpFormattedFiniteNumber(out, 3.5, "not finite", "bad format");
    RUVIA_CHECK_EQ(std::string(out.c_str()), std::string("3.5"));

    // NaN and both infinities are rejected rather than emitted as the words
    // "nan"/"inf", which are not valid SQL/RESP numeric literals and would splice
    // in unquoted. Nothing is appended on rejection.
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        std::pmr::string sink(std::pmr::get_default_resource());
        bool threw = false;
        try {
            ruvia::detail::appendHttpFormattedFiniteNumber(sink, bad, "not finite", "bad format");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK(sink.empty());
    }
}
