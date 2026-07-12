#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/model/FormParser.h"

namespace {

using ruvia::detail::parseFormBool;
using ruvia::detail::parseFormNumber;
using ruvia::detail::parseFormValue;

}  // namespace

RUVIA_TEST(form_bool_accepts_strict_set_only) {
    bool v = false;
    RUVIA_CHECK(parseFormBool("true", v) && v);
    RUVIA_CHECK(parseFormBool("1", v) && v);
    RUVIA_CHECK(parseFormBool("false", v) && !v);
    RUVIA_CHECK(parseFormBool("0", v) && !v);

    // Strict: no case-folding, no yes/on, no other numerics, no surrounding space.
    for (const std::string_view bad : {"TRUE", "False", "yes", "on", "2", "", " 1", "1 "}) {
        bool out = true;  // sentinel to prove reject leaves the target untouched
        RUVIA_CHECK(!parseFormBool(bad, out));
        RUVIA_CHECK(out);
    }
}

RUVIA_TEST(form_number_integer_is_strict) {
    int v = -1;
    RUVIA_CHECK(parseFormNumber("42", v) && v == 42);
    RUVIA_CHECK(parseFormNumber("-7", v) && v == -7);

    // Rejections: empty, trailing junk, fractional (no truncation to 1), overflow.
    RUVIA_CHECK(!parseFormNumber("", v));
    RUVIA_CHECK(!parseFormNumber("9x", v));
    RUVIA_CHECK(!parseFormNumber("1.5", v));
    RUVIA_CHECK(!parseFormNumber("99999999999999999999", v));

    // A negative cannot bind an unsigned target.
    unsigned u = 3;
    RUVIA_CHECK(!parseFormNumber("-5", u));
    RUVIA_CHECK(parseFormNumber("5", u) && u == 5u);
}

RUVIA_TEST(form_number_floating_accepts_fraction_and_exponent) {
    double v = 0;
    RUVIA_CHECK(parseFormNumber("1.5", v) && v == 1.5);
    RUVIA_CHECK(parseFormNumber("-2e3", v) && v == -2000.0);
    RUVIA_CHECK(!parseFormNumber("", v));
    RUVIA_CHECK(!parseFormNumber("1.2.3", v));  // trailing junk after a valid prefix
}

RUVIA_TEST(form_string_decode_failure_preserves_existing_value) {
    auto* resource = std::pmr::get_default_resource();
    ruvia::String value("original", resource);

    RUVIA_CHECK(!parseFormValue("decoded%2", value, resource));
    RUVIA_CHECK_EQ(value.view(), std::string_view("original"));

    RUVIA_CHECK(parseFormValue("decoded%20value", value, resource));
    RUVIA_CHECK_EQ(value.view(), std::string_view("decoded value"));
}
