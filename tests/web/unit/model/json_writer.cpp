#include "ruvia/web/detail/model/parse/json_writer.h"

#include <limits>
#include <memory_resource>
#include <string>

#include "ruvia/web/model_types.h"

#include "test_harness.h"

namespace {

template <typename t_type>
std::string to_json(const t_type& value) {
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::append_json_value(out, value);
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(json_writer_finite_numbers_and_bool) {
    RUVIA_CHECK_EQ(to_json(42), std::string("42"));
    RUVIA_CHECK_EQ(to_json(-7), std::string("-7"));
    RUVIA_CHECK_EQ(to_json(true), std::string("true"));
    RUVIA_CHECK_EQ(to_json(false), std::string("false"));
    RUVIA_CHECK_EQ(to_json(3.5), std::string("3.5"));
    RUVIA_CHECK_EQ(to_json(0.0), std::string("0"));
}

RUVIA_TEST(json_writer_non_finite_floats_become_null) {
    // RFC 8259 has no infinity/NaN; std::to_chars would emit "inf"/"nan"
    // (invalid JSON), so non-finite values serialize as null.
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    RUVIA_CHECK_EQ(to_json(inf), std::string("null"));
    RUVIA_CHECK_EQ(to_json(-inf), std::string("null"));
    RUVIA_CHECK_EQ(to_json(nan), std::string("null"));
}
