#include "test_harness.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/Error.h"
#include "ruvia/web/Validation.h"

namespace {

using ruvia::Validator;

}  // namespace

RUVIA_TEST(validator_required_flags_absent_values) {
    Validator v;
    std::optional<std::string> present = std::string("x");
    std::optional<std::string> absent;
    v.required(present, "present");
    v.required(absent, "absent", "absent is required");

    RUVIA_CHECK(!v.ok());
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(v.issues()[0].field(), std::string_view("absent"));
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("required"));
    RUVIA_CHECK_EQ(v.issues()[0].message(), std::string_view("absent is required"));
}

RUVIA_TEST(validator_length_bounds_and_absent_skips) {
    Validator v;
    std::optional<std::string> value = std::string("abc");
    v.minLength(value, "f", 2);  // 3 >= 2, ok
    v.maxLength(value, "f", 5);  // 3 <= 5, ok
    RUVIA_CHECK(v.ok());

    v.minLength(value, "f", 5);  // 3 < 5 -> min_length
    v.maxLength(value, "f", 2);  // 3 > 2 -> max_length
    // An absent value is never checked.
    std::optional<std::string> absent;
    v.minLength(absent, "g", 100);

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("min_length"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("max_length"));
}

RUVIA_TEST(validator_range_and_one_of) {
    Validator v;
    std::optional<int> n = 5;
    v.range(n, "n", 1, 10);  // in range, ok
    RUVIA_CHECK(v.ok());
    v.range(n, "n", 6, 10);  // 5 < 6 -> range

    std::optional<std::string> s = std::string("b");
    v.oneOf(s, "s", {"a", "b", "c"});  // allowed, ok
    v.oneOf(s, "s", {"x", "y"});       // not allowed -> one_of

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("range"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("one_of"));
}

RUVIA_TEST(validator_range_upper_bound_inclusive_and_absent_skips) {
    Validator v;
    // The upper bound is enforced independently of the lower bound.
    std::optional<int> high = 5;
    v.range(high, "high", 1, 3);  // 5 > 3 -> range
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("range"));
    // Both bounds are inclusive: values exactly at min or max are accepted.
    std::optional<int> atMin = 1;
    std::optional<int> atMax = 10;
    v.range(atMin, "atMin", 1, 10);
    v.range(atMax, "atMax", 1, 10);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    // An absent value skips range validation entirely.
    std::optional<int> absent;
    v.range(absent, "absent", 1, 3);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
}

RUVIA_TEST(validator_one_of_absent_skips_and_range_accepts_doubles) {
    Validator v;
    // oneOf on an absent optional is skipped -- the one rule whose absent-skip branch
    // the other tests don't exercise (required/minLength/range already cover theirs).
    std::optional<std::string> absent;
    v.oneOf(absent, "a", {"x", "y"});
    RUVIA_CHECK(v.ok());

    // range validates floating-point values, not just integers (a distinct template
    // instantiation and comparison path from the int cases above).
    std::optional<double> inRange = 0.5;
    v.range(inRange, "d", 0.0, 1.0);   // 0.0 <= 0.5 <= 1.0, ok
    RUVIA_CHECK(v.ok());
    std::optional<double> low = -0.1;
    v.range(low, "d", 0.0, 1.0);       // -0.1 < 0.0 -> range
    std::optional<double> high = 1.1;
    v.range(high, "d", 0.0, 1.0);      // 1.1 > 1.0 -> range
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("range"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("range"));

    // Both floating bounds are inclusive: a value exactly at min or max is accepted.
    std::optional<double> atMin = 0.0;
    std::optional<double> atMax = 1.0;
    v.range(atMin, "d", 0.0, 1.0);
    v.range(atMax, "d", 0.0, 1.0);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});  // unchanged
}

RUVIA_TEST(validation_error_serializes_issues_to_json) {
    Validator v;
    std::optional<std::string> absent;
    v.required(absent, "email", "email is required");
    std::optional<std::string> shortName = std::string("a");
    v.minLength(shortName, "name", 3, "too short");

    try {
        v.throwIfInvalid();
        RUVIA_CHECK(false);  // must have thrown
    } catch (const ruvia::ValidationError& error) {
        RUVIA_CHECK_EQ(
            error.info().detailsJson(),
            std::string_view(
                R"([{"field":"email","code":"required","message":"email is required"},)"
                R"({"field":"name","code":"min_length","message":"too short"}])"));
    }
}

RUVIA_TEST(validation_error_json_escapes_special_characters) {
    Validator v;
    // A field/message carrying a quote and backslash must be JSON-escaped so the
    // error body stays well-formed and cannot be broken out of.
    v.add("f\"x", "code", "a\"b\\c");
    try {
        v.throwIfInvalid();
        RUVIA_CHECK(false);
    } catch (const ruvia::ValidationError& error) {
        const auto json = error.info().detailsJson();
        RUVIA_CHECK(json.find(R"("field":"f\"x")") != std::string_view::npos);
        RUVIA_CHECK(json.find(R"("message":"a\"b\\c")") != std::string_view::npos);
    }
}

RUVIA_TEST(validator_throw_if_invalid_raises_on_issues) {
    Validator ok;
    ok.throwIfInvalid();  // no issues -> no throw

    Validator bad;
    std::optional<std::string> absent;
    bad.required(absent, "x");
    bool threw = false;
    try {
        bad.throwIfInvalid();
    } catch (const ruvia::ValidationError&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
