#include "test_harness.h"

#include <limits>
#include <memory_resource>
#include <string>

#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/JsonWriter.h"

namespace {

template <typename T>
std::string toJson(const T& value) {
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::appendJsonValue(out, value);
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(json_writer_finite_numbers_and_bool) {
    RUVIA_CHECK_EQ(toJson(42), std::string("42"));
    RUVIA_CHECK_EQ(toJson(-7), std::string("-7"));
    RUVIA_CHECK_EQ(toJson(true), std::string("true"));
    RUVIA_CHECK_EQ(toJson(false), std::string("false"));
    RUVIA_CHECK_EQ(toJson(3.5), std::string("3.5"));
    RUVIA_CHECK_EQ(toJson(0.0), std::string("0"));
}

RUVIA_TEST(json_writer_non_finite_floats_become_null) {
    // RFC 8259 has no infinity/NaN; std::to_chars would emit "inf"/"nan"
    // (invalid JSON), so non-finite values serialize as null.
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    RUVIA_CHECK_EQ(toJson(inf), std::string("null"));
    RUVIA_CHECK_EQ(toJson(-inf), std::string("null"));
    RUVIA_CHECK_EQ(toJson(nan), std::string("null"));
}
