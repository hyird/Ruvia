#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

namespace {

using ruvia::validator;
using ruvia::test::counting_memory_resource;

struct value_bound final {
    int value_;
};

struct bounded_value final {
    int value_;

    [[nodiscard]] bool operator<(value_bound bound) const noexcept {
        return value_ < bound.value_;
    }

    [[nodiscard]] bool operator>(value_bound bound) const noexcept {
        return value_ > bound.value_;
    }
};

RUVIA_MODEL(required_optional_model, RUVIA_REQUIRED_FIELD_NAME("requiredValue", required_value, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("optionalValue", optional_value, ruvia::string));

RUVIA_MODEL(precise_numeric_bounds,
    RUVIA_OPTIONAL_FIELD(unsigned_limit, ruvia::double_value,
        RUVIA_MAX(std::numeric_limits<std::uint64_t>::max(), "too large")),
    RUVIA_OPTIONAL_FIELD(signed_limit, ruvia::double_value,
        RUVIA_MIN(std::numeric_limits<std::int64_t>::min() + 1, "too small"),
        RUVIA_MAX(std::numeric_limits<std::int64_t>::max(), "too large")),
    RUVIA_OPTIONAL_FIELD(exact_minimum, ruvia::double_value,
        RUVIA_MIN(9007199254740993LL, "too small")));

RUVIA_MODEL(character_class_rules,
    RUVIA_OPTIONAL_FIELD(pattern_lower, ruvia::string, RUVIA_PATTERN(R"(^[\--0]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_not_lower, ruvia::string, RUVIA_PATTERN(R"(^[^\--0]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_upper, ruvia::string, RUVIA_PATTERN(R"(^[A-\]]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_not_upper, ruvia::string, RUVIA_PATTERN(R"(^[^A-\]]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_digits, ruvia::string, RUVIA_PATTERN(R"(^[\d]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_tail, ruvia::string, RUVIA_PATTERN("^[a-]$", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_escaped_dash, ruvia::string, RUVIA_PATTERN(R"(^[a\-z]$)", "invalid class")));
inline constexpr auto wide_class_pattern = ruvia::fixed_string{"^[^aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]*$"};

RUVIA_MODEL(bounded_pattern_rules,
    RUVIA_OPTIONAL_FIELD(wide_class, ruvia::string, RUVIA_PATTERN(wide_class_pattern, "invalid class")),
    RUVIA_OPTIONAL_FIELD(backtracking, ruvia::string,
        RUVIA_PATTERN("^a*a*a*b$", "pattern match limit exceeded")));

template <ruvia::fixed_string field_name>
void check_character_class_rule(std::string_view value, bool matches, std::string_view code) {
    character_class_rules model;
    model.set<field_name>(value);
    validator result;
    ruvia::validate_model(model, result);
    EXPECT_EQ(result.ok(), matches);
    EXPECT_EQ(result.issues().size(), matches ? std::size_t{0} : std::size_t{1});
    if (!result.issues().empty()) {
        EXPECT_EQ(result.issues()[0].field(), field_name.view());
        EXPECT_EQ(result.issues()[0].code(), code);
    }
}

[[nodiscard]] std::exception_ptr capture_validation_exception(std::pmr::memory_resource* resource) {
    validator validator_value({.resource_ = resource});
    validator_value.add(std::string(128, 'f'), "required", std::string(256, 'm'));
    try {
        validator_value.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation",
        });
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

[[nodiscard]] std::exception_ptr capture_validation_exception_from_short_lived_arena(
    std::pmr::memory_resource* upstream) {
    alignas(std::max_align_t) std::array<std::byte, 64> initial_value{};
    std::pmr::monotonic_buffer_resource arena(initial_value.data(), initial_value.size(), upstream);
    validator validator_value({.resource_ = &arena});
    validator_value.add(std::string(128, 'f'), "required", std::string(256, 'm'));
    try {
        validator_value.throw_if_invalid();
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

}  // namespace

TEST(Ruvia, request_model_required_and_optional_fields_are_structural) {
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<required_optional_model>("{}");
    }));

    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_form<required_optional_model>("", {.resource_ = std::pmr::get_default_resource()});
    }));
}

TEST(Ruvia, validator_required_flags_absent_values) {
    validator v;
    std::optional<std::string> present = std::string("x");
    std::optional<std::string> absent;
    v.required(present, "present");
    v.required(absent, "absent", "absent is required");

    EXPECT_TRUE(!v.ok());
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    EXPECT_EQ(v.issues()[0].field(), std::string_view("absent"));
    EXPECT_EQ(v.issues()[0].code(), std::string_view("required"));
    EXPECT_EQ(v.issues()[0].message(), std::string_view("absent is required"));
}

TEST(Ruvia, validator_length_bounds_and_absent_skips) {
    validator v;
    std::optional<std::string> value = std::string("abc");
    v.min_length(value, "f", 2);  // 3 >= 2, ok
    v.max_length(value, "f", 5);  // 3 <= 5, ok
    EXPECT_TRUE(v.ok());

    v.min_length(value, "f", 5);  // 3 < 5 -> too_small
    v.max_length(value, "f", 2);  // 3 > 2 -> too_big
    // An absent value is never checked.
    std::optional<std::string> absent;
    v.min_length(absent, "g", 100);

    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_and_one_of) {
    validator v;
    std::optional<int> n = 5;
    v.range(n, "n", 1, 10);  // in range, ok
    EXPECT_TRUE(v.ok());
    v.range(n, "n", 6, 10);  // 5 < 6 -> too_small

    std::optional<std::string> s = std::string("b");
    v.one_of(s, "s", {"a", "b", "c"});  // allowed, ok
    v.one_of(s, "s", {"x", "y"});       // not allowed -> one_of

    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("one_of"));
}

TEST(Ruvia, validator_range_upper_bound_inclusive_and_absent_skips) {
    validator v;
    // The upper bound is enforced independently of the lower bound.
    std::optional<int> high = 5;
    v.range(high, "high", 1, 3);  // 5 > 3 -> too_big
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_big"));
    // Both bounds are inclusive: values exactly at min or max are accepted.
    std::optional<int> at_min = 1;
    std::optional<int> at_max = 10;
    v.range(at_min, "atMin", 1, 10);
    v.range(at_max, "atMax", 1, 10);
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    // An absent value skips range validation entirely.
    std::optional<int> absent;
    v.range(absent, "absent", 1, 3);
    EXPECT_EQ(v.issues().size(), std::size_t{1});
}

TEST(Ruvia, validator_range_supports_heterogeneous_value_bounds) {
    const value_bound minimum{1};
    const value_bound maximum{10};
    validator valid;
    for (const int value : {1, 5, 10}) {
        valid.range(std::optional<bounded_value>{bounded_value{value}}, "custom", minimum, maximum);
    }
    valid.range(std::optional<bounded_value>{}, "absent", minimum, maximum);
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<bounded_value>{bounded_value{0}}, "below", minimum, maximum);
    invalid.range(std::optional<bounded_value>{bounded_value{11}}, "above", minimum, maximum);
    EXPECT_EQ(invalid.issues().size(), std::size_t{2});
    if (invalid.issues().size() != 2) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].field(), std::string_view("below"));
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].field(), std::string_view("above"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_compares_signed_and_unsigned_integers_without_conversion) {
    validator valid;
    valid.range(std::optional<std::uint64_t>{5}, "unsigned", -1, 10);
    valid.range(std::optional<std::int64_t>{5}, "signed", 0U, 10U);
    valid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<ruvia::uint64>{5}, "model_unsigned", -1, 10);
    valid.range(std::optional<int>{1}, "model_bounds", ruvia::int64{-1}, ruvia::uint64{10});
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<std::int64_t>{-1}, "negative", 0U, 10U);
    invalid.range(std::optional<std::uint64_t>{5}, "negative_upper", -10, -1);
    invalid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::int64_t>::max());
    invalid.range(std::optional<ruvia::int64>{-1}, "model_negative", 0U, 10U);
    EXPECT_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

TEST(Ruvia, validator_range_compares_floats_and_large_integers_exactly) {
    validator valid;
    valid.range(std::optional<double>{18446744073709549568.0}, "below_unsigned_max",
        0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<double>{-9223372036854775808.0}, "signed_minimum",
        std::numeric_limits<std::int64_t>::min(), 0);
    valid.range(std::optional<std::uint64_t>{9007199254740993ULL}, "above_float_minimum",
        9007199254740992.0, 9007199254740994.0);
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<double>{18446744073709551616.0}, "unsigned_max",
        0, std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<double>{9223372036854775808.0}, "signed_max",
        0, std::numeric_limits<std::int64_t>::max());
    invalid.range(std::optional<double>{-9223372036854775808.0}, "above_signed_minimum",
        std::numeric_limits<std::int64_t>::min() + 1, 0);
    invalid.range(std::optional<double>{9007199254740992.0}, "large_minimum",
        9007199254740993ULL, std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<std::uint64_t>{9007199254740993ULL}, "integer_maximum",
        0.0, 9007199254740992.0);
    EXPECT_EQ(invalid.issues().size(), std::size_t{5});
    if (invalid.issues().size() != 5) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[4].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_compares_fractional_bounds_and_nonfinite_values) {
    const auto infinity = std::numeric_limits<double>::infinity();
    validator valid;
    valid.range(std::optional<int>{-1}, "negative", -1.5, -0.5);
    valid.range(std::optional<unsigned>{0}, "zero", -0.5, 0.5);
    valid.range(std::optional<int>{1}, "positive", 0.5, 1.5);
    valid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "infinite_bounds", -infinity, infinity);
    valid.range(std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
        "unordered", std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<unsigned>{0}, "positive_minimum", 0.5, 1.0);
    invalid.range(std::optional<int>{0}, "negative_maximum", -1.0, -0.5);
    invalid.range(std::optional<double>{infinity}, "positive_infinity", 0,
        std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<double>{-infinity}, "negative_infinity",
        std::numeric_limits<std::int64_t>::min(), 0);
    EXPECT_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

TEST(Ruvia, model_numeric_rules_compare_float_fields_and_integer_bounds_exactly) {
    for (const auto body : {R"({"unsigned_limit":18446744073709549568})",
             R"({"signed_limit":9223372036854774784})",
             R"({"signed_limit":-9223372036854774784})",
             R"({"exact_minimum":9007199254740994})"}) {
        auto value = ruvia::from_json<precise_numeric_bounds>(body);
        validator result;
        ruvia::validate_model(value, result);
        EXPECT_TRUE(result.ok());
    }
    for (const auto& [body, code] : {
             std::pair{R"({"unsigned_limit":18446744073709551616})", "too_big"},
             std::pair{R"({"signed_limit":9223372036854775808})", "too_big"},
             std::pair{R"({"signed_limit":-9223372036854775808})", "too_small"},
             std::pair{R"({"exact_minimum":9007199254740992})", "too_small"}}) {
        bool rejected = false;
        try {
            (void)ruvia::from_json<precise_numeric_bounds>(body);
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (!error.validation_issues().empty()) {
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view(code));
            }
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, model_pattern_rules_match_escaped_character_ranges) {
    for (const auto& [value, matches] : {
             std::pair{"-", true}, std::pair{".", true}, std::pair{"/", true}, std::pair{"0", true},
             std::pair{",", false}, std::pair{"1", false}, std::pair{"a", false}}) {
        check_character_class_rule<"pattern_lower">(value, matches, "pattern");
        check_character_class_rule<"pattern_not_lower">(value, !matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"A", true}, std::pair{"Z", true}, std::pair{"\\", true}, std::pair{"]", true},
             std::pair{"@", false}, std::pair{"^", false}, std::pair{"a", false}}) {
        check_character_class_rule<"pattern_upper">(value, matches, "pattern");
        check_character_class_rule<"pattern_not_upper">(value, !matches, "pattern");
    }
}

TEST(Ruvia, model_pattern_rules_preserve_class_escapes_and_literal_hyphens) {
    for (const auto& [value, matches] : {
             std::pair{"0", true}, std::pair{"9", true}, std::pair{"d", false}, std::pair{"a", false},
             std::pair{"-", false}, std::pair{"00", false}}) {
        check_character_class_rule<"pattern_digits">(value, matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"a", true}, std::pair{"-", true}, std::pair{"b", false}, std::pair{"z", false}}) {
        check_character_class_rule<"pattern_tail">(value, matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"a", true}, std::pair{"-", true}, std::pair{"z", true}, std::pair{"m", false}}) {
        check_character_class_rule<"pattern_escaped_dash">(value, matches, "pattern");
    }
}

TEST(Ruvia, model_pattern_rules_bound_matching_work) {
    // A class costs one lookup per input byte however many members it lists.
    bounded_pattern_rules model;
    model.set<"wide_class">(std::string(4096, 'b'));
    validator class_result;
    ruvia::validate_model(model, class_result);
    EXPECT_TRUE(class_result.ok());
    model.set<"wide_class">("ba");
    validator class_mismatch;
    ruvia::validate_model(model, class_mismatch);
    EXPECT_EQ(class_mismatch.issues().size(), std::size_t{1});
    model.reset<"wide_class">();

    model.set<"backtracking">("aaab");
    validator short_result;
    ruvia::validate_model(model, short_result);
    EXPECT_TRUE(short_result.ok());

    // Cubic backtracking over a long non-matching input stops at the shared
    // matching work limit instead of running unbounded, and fails closed.
    model.set<"backtracking">(std::string(4096, 'a'));
    validator long_result;
    ruvia::validate_model(model, long_result);
    EXPECT_EQ(long_result.issues().size(), std::size_t{1});
    if (long_result.issues().size() == 1) {
        EXPECT_EQ(long_result.issues()[0].field(), std::string_view("backtracking"));
        EXPECT_EQ(long_result.issues()[0].code(), std::string_view("pattern"));
        EXPECT_EQ(long_result.issues()[0].message(), std::string_view("pattern match limit exceeded"));
    }
}

TEST(Ruvia, validator_one_of_absent_skips_and_range_accepts_doubles) {
    validator v;
    // one_of on an absent optional is skipped -- the one rule whose absent-skip branch
    // the other tests don't exercise (required/min_length/range already cover theirs).
    std::optional<std::string> absent;
    v.one_of(absent, "a", {"x", "y"});
    EXPECT_TRUE(v.ok());

    // range validates floating-point values, not just integers (a distinct template
    // instantiation and comparison path from the int cases above).
    std::optional<double> in_range = 0.5;
    v.range(in_range, "d", 0.0, 1.0);  // 0.0 <= 0.5 <= 1.0, ok
    EXPECT_TRUE(v.ok());
    std::optional<double> low = -0.1;
    v.range(low, "d", 0.0, 1.0);  // -0.1 < 0.0 -> too_small
    std::optional<double> high = 1.1;
    v.range(high, "d", 0.0, 1.0);  // 1.1 > 1.0 -> too_big
    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("too_big"));

    // Both floating bounds are inclusive: a value exactly at min or max is accepted.
    std::optional<double> at_min = 0.0;
    std::optional<double> at_max = 1.0;
    v.range(at_min, "d", 0.0, 1.0);
    v.range(at_max, "d", 0.0, 1.0);
    EXPECT_EQ(v.issues().size(), std::size_t{2});  // unchanged
}

TEST(Ruvia, throw_if_invalid_exposes_typed_issues) {
    validator v;
    std::optional<std::string> absent;
    v.required(absent, "email", "email is required");
    std::optional<std::string> short_name = std::string("a");
    v.min_length(short_name, "name", 3, "too short");

    try {
        v.throw_if_invalid();
        EXPECT_TRUE(false);  // must have thrown
    } catch (const ruvia::http_error& error) {
        const auto issues = error.info().validation_issues();
        EXPECT_EQ(issues.size(), std::size_t{2});
        EXPECT_EQ(issues[0].field(), std::string_view("email"));
        EXPECT_EQ(issues[0].code(), std::string_view("required"));
        EXPECT_EQ(issues[0].message(), std::string_view("email is required"));
        EXPECT_EQ(issues[1].field(), std::string_view("name"));
        EXPECT_EQ(issues[1].code(), std::string_view("too_small"));
        EXPECT_EQ(issues[1].message(), std::string_view("too short"));
    }
}

TEST(Ruvia, throw_if_invalid_preserves_special_characters_as_typed_data) {
    validator v;
    v.add("f\"x", "code", "a\"b\\c");
    try {
        v.throw_if_invalid();
        EXPECT_TRUE(false);
    } catch (const ruvia::http_error& error) {
        const auto issues = error.info().validation_issues();
        EXPECT_EQ(issues.size(), std::size_t{1});
        EXPECT_EQ(issues[0].field(), std::string_view("f\"x"));
        EXPECT_EQ(issues[0].message(), std::string_view("a\"b\\c"));
    }
}

TEST(Ruvia, validation_failure_options_control_reported_error_info) {
    validator v;
    v.add("field", "required", "missing");

    try {
        v.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation",
        });
        EXPECT_TRUE(false);
    } catch (const ruvia::http_error& error) {
        const auto info = error.info();
        EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
        EXPECT_EQ(info.code(), std::string_view("invalid_payload"));
        EXPECT_EQ(info.message(), std::string_view("payload failed validation"));
        EXPECT_EQ(info.validation_issues().size(), std::size_t{1});
    }
}

TEST(Ruvia, throw_if_invalid_releases_validator_resource) {
    counting_memory_resource resource;
    const auto exception = capture_validation_exception(&resource);

    EXPECT_TRUE(exception != nullptr);
    EXPECT_TRUE(resource.allocation_count() > 0);
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
    EXPECT_EQ(resource.allocation_count(), resource.deallocation_count());

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::http_error& error) {
        EXPECT_EQ(std::string_view(error.what()), std::string_view("payload failed validation"));
        EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
        EXPECT_EQ(error.validation_issues()[0].field().size(), std::size_t{128});
        EXPECT_EQ(error.validation_issues()[0].message().size(), std::size_t{256});
    }
}

TEST(Ruvia, throw_if_invalid_survives_short_lived_arena_and_exception_ptr) {
    counting_memory_resource upstream;
    const auto exception = capture_validation_exception_from_short_lived_arena(&upstream);

    // The arena, its initial buffer, and the Validator have all gone away before
    // the exception is inspected. The exception owns process-lifetime copies.
    EXPECT_TRUE(exception != nullptr);
    EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
    EXPECT_EQ(upstream.allocation_count(), upstream.deallocation_count());

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::http_error& error) {
        EXPECT_EQ(std::string_view(error.what()),
            std::string_view("request validation failed"));
        EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
        EXPECT_EQ(error.validation_issues()[0].field().size(), std::size_t{128});
        EXPECT_EQ(error.validation_issues()[0].message().size(), std::size_t{256});
    }
}

TEST(Ruvia, http_error_copy_move_and_assignment_preserve_owned_issues) {
    counting_memory_resource resource;
    const auto exception = capture_validation_exception(&resource);

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::http_error& original) {
        ruvia::http_error copied(original);
        ruvia::http_error copy_assigned({.status_ = ruvia::http_status::bad_request});
        copy_assigned = original;

        ruvia::http_error moved(std::move(copied));
        ruvia::http_error move_assigned({.status_ = ruvia::http_status::bad_request});
        move_assigned = std::move(moved);

        for (const auto* error : {&copy_assigned, &move_assigned}) {
            EXPECT_EQ(error->validation_issues().size(), std::size_t{1});
            EXPECT_EQ(error->validation_issues()[0].field().size(), std::size_t{128});
            EXPECT_EQ(error->validation_issues()[0].message().size(), std::size_t{256});
            EXPECT_EQ(error->info().status(), ruvia::http_status::unprocessable_content);
            EXPECT_EQ(error->info().code(), std::string_view("invalid_payload"));
            EXPECT_EQ(std::string_view(error->what()),
                std::string_view("payload failed validation"));
        }
        EXPECT_EQ(copied.validation_issues().size(), std::size_t{0});
        EXPECT_EQ(moved.validation_issues().size(), std::size_t{0});
    }

    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, validator_throw_if_invalid_raises_on_issues) {
    validator ok;
    ok.throw_if_invalid();  // no issues -> no throw

    validator bad;
    std::optional<std::string> absent;
    bad.required(absent, "x");
    bool threw = false;
    try {
        bad.throw_if_invalid();
    } catch (const ruvia::http_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, validation_diagnostic_text_preserves_complete_utf8_within_limit) {
    ruvia::validator validator;
    std::string message(ruvia::max_validation_text_bytes - 1, 'x');
    message += "中文";
    validator.add("field", "code", message);
    EXPECT_EQ(validator.issues()[0].message().size(), ruvia::max_validation_text_bytes - 1);
    EXPECT_EQ(validator.issues()[0].message(), std::string_view(message).substr(0, ruvia::max_validation_text_bytes - 1));
}

namespace validator_ownership_test {

void add_owned_issues(ruvia::validator& value) {
    value.add(std::string(128, 'f'), std::string(96, 'c'), std::string(256, 'm'));
    value.add("secondary", "invalid", "second issue");
}

void check_owned_issues(const ruvia::validator& value,
    std::pmr::memory_resource* resource, std::size_t count = 2) {
    EXPECT_TRUE(value.resource() == resource);
    EXPECT_TRUE(value.issues().get_allocator().resource() == resource);
    EXPECT_EQ(value.issues().size(), count);
    EXPECT_TRUE(!value.ok());
    if (value.issues().size() >= 2) {
        EXPECT_EQ(value.issues()[0].field(), std::string_view(std::string(128, 'f')));
        EXPECT_EQ(value.issues()[0].code(), std::string_view(std::string(96, 'c')));
        EXPECT_EQ(value.issues()[0].message(), std::string_view(std::string(256, 'm')));
        EXPECT_EQ(value.issues()[1].field(), std::string_view("secondary"));
        EXPECT_EQ(value.issues()[1].code(), std::string_view("invalid"));
        EXPECT_EQ(value.issues()[1].message(), std::string_view("second issue"));
    }
}

void check_owned_error(const ruvia::validator& value,
    std::size_t count = 2) {
    bool rejected = false;
    try {
        value.throw_if_invalid({.status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation"});
    } catch (const ruvia::http_error& error) {
        rejected = true;
        EXPECT_EQ(error.info().status(), ruvia::http_status::unprocessable_content);
        EXPECT_EQ(error.info().code(), std::string_view("invalid_payload"));
        EXPECT_EQ(std::string_view(error.what()), std::string_view("payload failed validation"));
        EXPECT_EQ(error.validation_issues().size(), count);
        if (error.validation_issues().size() >= 2) {
            EXPECT_EQ(error.validation_issues()[0].field(), std::string_view(std::string(128, 'f')));
            EXPECT_EQ(error.validation_issues()[0].message(), std::string_view(std::string(256, 'm')));
            EXPECT_EQ(error.validation_issues()[1].field(), std::string_view("secondary"));
        }
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, validator_copy_constructor_preserves_resource_and_owns_diagnostics) {
    ruvia::test::counting_memory_resource upstream;
    {
        ruvia::test::equivalent_tracking_resource resource(upstream);
        ruvia::test::equivalent_tracking_resource temporary_default(upstream);
        std::optional<ruvia::validator> copy;
        {
            ruvia::validator input({.resource_ = &resource});
            add_owned_issues(input);
            const ruvia::test::default_resource_guard default_resource(temporary_default);
            copy.emplace(input);
            check_owned_issues(input, &resource);
            check_owned_issues(*copy, &resource);
        }
        temporary_default.retire();
        check_owned_issues(*copy, &resource);
        copy->add("later", "required", std::string(200, 'a'));
        check_owned_issues(*copy, &resource, 3);
        EXPECT_EQ(copy->issues().back().message(), std::string_view(std::string(200, 'a')));
        check_owned_error(*copy, 3);
        copy.reset();
        EXPECT_EQ(temporary_default.callbacks_after_retirement(), std::size_t{0});
    }
    EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
    EXPECT_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

TEST(Ruvia, validator_assignment_retains_destination_resource_after_source_retirement) {
    for (const bool move : {false, true}) {
        ruvia::test::counting_memory_resource upstream;
        {
            ruvia::test::equivalent_tracking_resource source(upstream);
            ruvia::test::equivalent_tracking_resource target(upstream);
            EXPECT_TRUE(source.is_equal(target));
            std::optional<ruvia::validator> destination(std::in_place, ruvia::model_options{.resource_ = &target});
            destination->add("seed", "old", std::string(180, 's'));
            {
                ruvia::validator input({.resource_ = &source});
                add_owned_issues(input);
                const ruvia::test::default_resource_guard default_resource(source);
                if (move) {
                    *destination = std::move(input);
                    EXPECT_TRUE(input.ok());
                    EXPECT_TRUE(input.resource() == &source);
                    EXPECT_TRUE(input.issues().get_allocator().resource() == &source);
                    input.add("reused", "required", std::string(190, 'r'));
                    EXPECT_EQ(input.issues().size(), std::size_t{1});
                    EXPECT_EQ(input.issues()[0].message(), std::string_view(std::string(190, 'r')));
                } else {
                    *destination = input;
                    check_owned_issues(input, &source);
                }
                check_owned_issues(*destination, &target);
            }
            // Keep the retired resource alive so stale callbacks are observable
            // without invoking a destroyed allocator object in the red run.
            source.retire();
            check_owned_issues(*destination, &target);
            destination->add("later", "required", std::string(200, 'a'));
            check_owned_issues(*destination, &target, 3);
            EXPECT_EQ(destination->issues().back().message(), std::string_view(std::string(200, 'a')));
            check_owned_error(*destination, 3);
            destination.reset();
            EXPECT_EQ(source.callbacks_after_retirement(), std::size_t{0});
        }
        EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
        EXPECT_EQ(upstream.allocation_count(), upstream.deallocation_count());
    }
}

TEST(Ruvia, validator_copy_and_rebinding_failure_preserve_diagnostics_and_resources) {
    for (const bool move : {false, true}) {
        for (const auto allowance : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
            ruvia::test::counting_memory_resource upstream;
            {
                ruvia::test::equivalent_tracking_resource source(upstream);
                ruvia::test::equivalent_tracking_resource target(upstream);
                ruvia::validator input({.resource_ = &source});
                add_owned_issues(input);
                ruvia::validator destination({.resource_ = &target});
                destination.add("seed", "old", std::string(180, 's'));
                const auto live = upstream.live_allocations();
                target.reject_allocations_after(allowance);
                bool rejected = false;
                try {
                    if (move) {
                        destination = std::move(input);
                    } else {
                        destination = input;
                    }
                } catch (const std::bad_alloc&) {
                    rejected = true;
                }
                target.allow_allocations();
                EXPECT_TRUE(rejected);
                EXPECT_EQ(upstream.live_allocations(), live);
                check_owned_issues(input, &source);
                EXPECT_TRUE(destination.resource() == &target);
                EXPECT_TRUE(destination.issues().get_allocator().resource() == &target);
                EXPECT_EQ(destination.issues().size(), std::size_t{1});
                if (!destination.issues().empty()) {
                    EXPECT_EQ(destination.issues()[0].field(), std::string_view("seed"));
                    EXPECT_EQ(destination.issues()[0].code(), std::string_view("old"));
                    EXPECT_EQ(destination.issues()[0].message(), std::string_view(std::string(180, 's')));
                }
                source.reject_allocations_after(allowance);
                rejected = false;
                try {
                    const ruvia::validator copied(input);
                    check_owned_issues(copied, &source);
                } catch (const std::bad_alloc&) {
                    rejected = true;
                }
                source.allow_allocations();
                EXPECT_TRUE(rejected);
                EXPECT_EQ(upstream.live_allocations(), live);
                check_owned_issues(input, &source);
                input.add("source remains usable", "required", std::string(200, 'a'));
                destination.add("target remains usable", "required", std::string(200, 'b'));
                EXPECT_EQ(input.issues().size(), std::size_t{3});
                EXPECT_EQ(destination.issues().size(), std::size_t{2});
                EXPECT_EQ(input.issues().back().message(), std::string_view(std::string(200, 'a')));
                EXPECT_EQ(destination.issues().back().message(), std::string_view(std::string(200, 'b')));
            }
            EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
            EXPECT_EQ(upstream.allocation_count(), upstream.deallocation_count());
        }
    }
}

TEST(Ruvia, validator_same_resource_moves_and_self_assignment_do_not_allocate) {
    ruvia::test::counting_memory_resource upstream;
    {
        ruvia::test::equivalent_tracking_resource resource(upstream);
        ruvia::validator input({.resource_ = &resource});
        add_owned_issues(input);
        ruvia::validator destination({.resource_ = &resource});
        destination.add("seed", "old", std::string(180, 's'));
        std::optional<ruvia::validator> moved;
        const auto attempts = resource.allocation_attempts();
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            resource.reject_allocations_after(0);
        }
        bool rejected = false;
        try {
            moved.emplace(std::move(input));
            check_owned_issues(*moved, &resource);
            destination = std::move(*moved);
            auto* alias = std::addressof(destination);
            destination = *alias;
            destination = std::move(*alias);
        } catch (const std::bad_alloc&) {
            rejected = true;
        }
        resource.allow_allocations();
        EXPECT_TRUE(!rejected);
        if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
            EXPECT_EQ(resource.allocation_attempts(), attempts);
        }
        check_owned_issues(destination, &resource);
        EXPECT_TRUE(input.ok());
        EXPECT_TRUE(input.resource() == &resource);
        EXPECT_TRUE(input.issues().get_allocator().resource() == &resource);
        if (moved) {
            EXPECT_TRUE(moved->ok());
            EXPECT_TRUE(moved->resource() == &resource);
            EXPECT_TRUE(moved->issues().get_allocator().resource() == &resource);
            moved->add("reused after assignment", "required", std::string(200, 'b'));
            EXPECT_EQ(moved->issues().size(), std::size_t{1});
            EXPECT_EQ(moved->issues()[0].message(), std::string_view(std::string(200, 'b')));
        }
        input.add("reused after construction", "required", std::string(200, 'a'));
        EXPECT_EQ(input.issues().size(), std::size_t{1});
        EXPECT_EQ(input.issues()[0].message(), std::string_view(std::string(200, 'a')));
        check_owned_error(destination);
    }
    EXPECT_EQ(upstream.live_allocations(), std::size_t{0});
    EXPECT_EQ(upstream.allocation_count(), upstream.deallocation_count());
}

}  // namespace validator_ownership_test

namespace {

RUVIA_MODEL(lazy_path_item, RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_MIN(0, "must not be negative")));

RUVIA_MODEL(lazy_path_envelope,
    RUVIA_REQUIRED_FIELD_NAME("field_name_longer_than_small_string_storage", label, ruvia::string,
        RUVIA_MIN(1, "must not be empty")),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<lazy_path_item>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<lazy_path_item>));

}  // namespace

TEST(Ruvia, validator_model_paths_are_built_only_for_emitted_issues) {
    std::string body = R"({"field_name_longer_than_small_string_storage":"x","items":[)";
    for (std::size_t index = 0; index != 1024; ++index) {
        body.append(index == 0 ? R"({"value":1})" : R"(,{"value":1})");
    }
    body.append(R"(],"boxed":[{"value":1},{"value":2}]})");
    auto model = ruvia::from_json<lazy_path_envelope>(body);
    counting_memory_resource resource;
    {
        validator result({.resource_ = &resource});
        const auto baseline = resource.allocation_count();
        ruvia::validate_model(model, result);
        EXPECT_TRUE(result.ok());
        EXPECT_EQ(resource.allocation_count(), baseline);

        model.set<"label">("");
        model.ensure<"items">()[700].set<"value">(-1);
        model.ensure<"boxed">()[1].set<"value">(-2);
        ruvia::validate_model(model, result);
        EXPECT_EQ(result.issues().size(), std::size_t{3});
        if (result.issues().size() == 3) {
            EXPECT_EQ(result.issues()[0].field(), std::string_view("field_name_longer_than_small_string_storage"));
            EXPECT_EQ(result.issues()[1].field(), std::string_view("items[700].value"));
            EXPECT_EQ(result.issues()[2].field(), std::string_view("boxed[1].value"));
            for (const auto& issue : result.issues()) {
                EXPECT_EQ(issue.code(), std::string_view("too_small"));
            }
        }
    }
    EXPECT_EQ(resource.live_allocations(), std::size_t{0});
}

TEST(Ruvia, validator_retains_all_model_validation_issues_beyond_64) {
    lazy_path_envelope model;
    model.set<"label">("ok");
    auto& items = model.ensure<"items">();
    constexpr std::size_t invalid_count = 80;
    for (std::size_t index = 0; index < invalid_count; ++index) {
        items.emplace_back().set<"value">(-1);
    }
    validator result;
    ruvia::validate_model(model, result);
    EXPECT_EQ(result.issues().size(), invalid_count);
    if (result.issues().size() == invalid_count) {
        EXPECT_EQ(result.issues().front().field(), std::string_view("items[0].value"));
        EXPECT_EQ(result.issues().back().field(), std::string_view("items[79].value"));
    }
}

namespace {

RUVIA_MODEL(email_rule_model,
    RUVIA_REQUIRED_FIELD(mail, ruvia::string, RUVIA_EMAIL("invalid email")));

}  // namespace

TEST(Ruvia, model_email_rule_rejects_empty_domain_labels_and_multiple_at_signs) {
    for (const auto* valid : {
             R"({"mail":"user@example.com"})",
             R"({"mail":"first.last@sub.example.co.uk"})",
             R"({"mail":"用户@例子.测试"})",
         }) {
        const auto parsed = ruvia::from_json<email_rule_model>(valid);
        validator result;
        ruvia::validate_model(parsed, result);
        EXPECT_TRUE(result.ok());
    }

    for (const auto* invalid : {
             R"({"mail":"a@.b"})",
             R"({"mail":"a@b.c."})",
             R"({"mail":"a@b..c"})",
             R"({"mail":"a@@b.c"})",
             R"({"mail":"a@b@c.d"})",
             R"({"mail":"@example.com"})",
             R"({"mail":"user@example"})",
         }) {
        bool rejected = false;
        try {
            (void)ruvia::from_json<email_rule_model>(invalid);
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (error.validation_issues().size() == 1) {
                EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("mail"));
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("email"));
            }
        }
        EXPECT_TRUE(rejected);
    }
}
