#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/request_validation.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

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

RUVIA_TEST(request_model_required_and_optional_fields_are_structural) {
    RUVIA_CHECK(!ruvia::from_json<required_optional_model>("{}").has_value());

    RUVIA_CHECK(
        !ruvia::from_form<required_optional_model>("", {.resource_ = std::pmr::get_default_resource()})
            .has_value());
}

RUVIA_TEST(validator_required_flags_absent_values) {
    validator v;
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
    validator v;
    std::optional<std::string> value = std::string("abc");
    v.min_length(value, "f", 2);  // 3 >= 2, ok
    v.max_length(value, "f", 5);  // 3 <= 5, ok
    RUVIA_CHECK(v.ok());

    v.min_length(value, "f", 5);  // 3 < 5 -> too_small
    v.max_length(value, "f", 2);  // 3 > 2 -> too_big
    // An absent value is never checked.
    std::optional<std::string> absent;
    v.min_length(absent, "g", 100);

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("too_big"));
}

RUVIA_TEST(validator_range_and_one_of) {
    validator v;
    std::optional<int> n = 5;
    v.range(n, "n", 1, 10);  // in range, ok
    RUVIA_CHECK(v.ok());
    v.range(n, "n", 6, 10);  // 5 < 6 -> too_small

    std::optional<std::string> s = std::string("b");
    v.one_of(s, "s", {"a", "b", "c"});  // allowed, ok
    v.one_of(s, "s", {"x", "y"});       // not allowed -> one_of

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("one_of"));
}

RUVIA_TEST(validator_range_upper_bound_inclusive_and_absent_skips) {
    validator v;
    // The upper bound is enforced independently of the lower bound.
    std::optional<int> high = 5;
    v.range(high, "high", 1, 3);  // 5 > 3 -> too_big
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_big"));
    // Both bounds are inclusive: values exactly at min or max are accepted.
    std::optional<int> at_min = 1;
    std::optional<int> at_max = 10;
    v.range(at_min, "atMin", 1, 10);
    v.range(at_max, "atMax", 1, 10);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    // An absent value skips range validation entirely.
    std::optional<int> absent;
    v.range(absent, "absent", 1, 3);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
}

RUVIA_TEST(validator_range_supports_heterogeneous_value_bounds) {
    const value_bound minimum{1};
    const value_bound maximum{10};
    validator valid;
    for (const int value : {1, 5, 10}) {
        valid.range(std::optional<bounded_value>{bounded_value{value}}, "custom", minimum, maximum);
    }
    valid.range(std::optional<bounded_value>{}, "absent", minimum, maximum);
    RUVIA_CHECK(valid.ok());

    validator invalid;
    invalid.range(std::optional<bounded_value>{bounded_value{0}}, "below", minimum, maximum);
    invalid.range(std::optional<bounded_value>{bounded_value{11}}, "above", minimum, maximum);
    RUVIA_CHECK_EQ(invalid.issues().size(), std::size_t{2});
    if (invalid.issues().size() != 2) {
        return;
    }
    RUVIA_CHECK_EQ(invalid.issues()[0].field(), std::string_view("below"));
    RUVIA_CHECK_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(invalid.issues()[1].field(), std::string_view("above"));
    RUVIA_CHECK_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
}

RUVIA_TEST(validator_range_compares_signed_and_unsigned_integers_without_conversion) {
    validator valid;
    valid.range(std::optional<std::uint64_t>{5}, "unsigned", -1, 10);
    valid.range(std::optional<std::int64_t>{5}, "signed", 0U, 10U);
    valid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<ruvia::uint64>{5}, "model_unsigned", -1, 10);
    valid.range(std::optional<int>{1}, "model_bounds", ruvia::int64{-1}, ruvia::uint64{10});
    RUVIA_CHECK(valid.ok());

    validator invalid;
    invalid.range(std::optional<std::int64_t>{-1}, "negative", 0U, 10U);
    invalid.range(std::optional<std::uint64_t>{5}, "negative_upper", -10, -1);
    invalid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::int64_t>::max());
    invalid.range(std::optional<ruvia::int64>{-1}, "model_negative", 0U, 10U);
    RUVIA_CHECK_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    RUVIA_CHECK_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

RUVIA_TEST(validator_range_compares_floats_and_large_integers_exactly) {
    validator valid;
    valid.range(std::optional<double>{18446744073709549568.0}, "below_unsigned_max",
        0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<double>{-9223372036854775808.0}, "signed_minimum",
        std::numeric_limits<std::int64_t>::min(), 0);
    valid.range(std::optional<std::uint64_t>{9007199254740993ULL}, "above_float_minimum",
        9007199254740992.0, 9007199254740994.0);
    RUVIA_CHECK(valid.ok());

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
    RUVIA_CHECK_EQ(invalid.issues().size(), std::size_t{5});
    if (invalid.issues().size() != 5) {
        return;
    }
    RUVIA_CHECK_EQ(invalid.issues()[0].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[2].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(invalid.issues()[4].code(), std::string_view("too_big"));
}

RUVIA_TEST(validator_range_compares_fractional_bounds_and_nonfinite_values) {
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
    RUVIA_CHECK(valid.ok());

    validator invalid;
    invalid.range(std::optional<unsigned>{0}, "positive_minimum", 0.5, 1.0);
    invalid.range(std::optional<int>{0}, "negative_maximum", -1.0, -0.5);
    invalid.range(std::optional<double>{infinity}, "positive_infinity", 0,
        std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<double>{-infinity}, "negative_infinity",
        std::numeric_limits<std::int64_t>::min(), 0);
    RUVIA_CHECK_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    RUVIA_CHECK_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    RUVIA_CHECK_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

RUVIA_TEST(model_numeric_rules_compare_float_fields_and_integer_bounds_exactly) {
    const ruvia::json_body<precise_numeric_bounds> rules;
    for (const auto body : {R"({"unsigned_limit":18446744073709549568})",
             R"({"signed_limit":9223372036854774784})",
             R"({"signed_limit":-9223372036854774784})",
             R"({"exact_minimum":9007199254740994})"}) {
        auto value = ruvia::from_json<precise_numeric_bounds>(body);
        RUVIA_CHECK(value.has_value());
        if (!value) {
            return;
        }
        validator result;
        rules.validate(*value, result);
        RUVIA_CHECK(result.ok());
    }
    for (const auto& [body, code] : {
             std::pair{R"({"unsigned_limit":18446744073709551616})", "too_big"},
             std::pair{R"({"signed_limit":9223372036854775808})", "too_big"},
             std::pair{R"({"signed_limit":-9223372036854775808})", "too_small"},
             std::pair{R"({"exact_minimum":9007199254740992})", "too_small"}}) {
        auto value = ruvia::from_json<precise_numeric_bounds>(body);
        RUVIA_CHECK(value.has_value());
        if (!value) {
            return;
        }
        validator result;
        rules.validate(*value, result);
        RUVIA_CHECK_EQ(result.issues().size(), std::size_t{1});
        if (result.issues().empty()) {
            continue;
        }
        RUVIA_CHECK_EQ(result.issues()[0].code(), std::string_view(code));
    }
}

RUVIA_TEST(validator_one_of_absent_skips_and_range_accepts_doubles) {
    validator v;
    // one_of on an absent optional is skipped -- the one rule whose absent-skip branch
    // the other tests don't exercise (required/min_length/range already cover theirs).
    std::optional<std::string> absent;
    v.one_of(absent, "a", {"x", "y"});
    RUVIA_CHECK(v.ok());

    // range validates floating-point values, not just integers (a distinct template
    // instantiation and comparison path from the int cases above).
    std::optional<double> in_range = 0.5;
    v.range(in_range, "d", 0.0, 1.0);  // 0.0 <= 0.5 <= 1.0, ok
    RUVIA_CHECK(v.ok());
    std::optional<double> low = -0.1;
    v.range(low, "d", 0.0, 1.0);  // -0.1 < 0.0 -> too_small
    std::optional<double> high = 1.1;
    v.range(high, "d", 0.0, 1.0);  // 1.1 > 1.0 -> too_big
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("too_big"));

    // Both floating bounds are inclusive: a value exactly at min or max is accepted.
    std::optional<double> at_min = 0.0;
    std::optional<double> at_max = 1.0;
    v.range(at_min, "d", 0.0, 1.0);
    v.range(at_max, "d", 0.0, 1.0);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});  // unchanged
}

RUVIA_TEST(validation_error_exposes_typed_issues) {
    validator v;
    std::optional<std::string> absent;
    v.required(absent, "email", "email is required");
    std::optional<std::string> short_name = std::string("a");
    v.min_length(short_name, "name", 3, "too short");

    try {
        v.throw_if_invalid();
        RUVIA_CHECK(false);  // must have thrown
    } catch (const ruvia::validation_error& error) {
        const auto issues = error.info().validation_issues();
        RUVIA_CHECK_EQ(issues.size(), std::size_t{2});
        RUVIA_CHECK_EQ(issues[0].field(), std::string_view("email"));
        RUVIA_CHECK_EQ(issues[0].code(), std::string_view("required"));
        RUVIA_CHECK_EQ(issues[0].message(), std::string_view("email is required"));
        RUVIA_CHECK_EQ(issues[1].field(), std::string_view("name"));
        RUVIA_CHECK_EQ(issues[1].code(), std::string_view("too_small"));
        RUVIA_CHECK_EQ(issues[1].message(), std::string_view("too short"));
    }
}

RUVIA_TEST(validation_error_preserves_special_characters_as_typed_data) {
    validator v;
    v.add("f\"x", "code", "a\"b\\c");
    try {
        v.throw_if_invalid();
        RUVIA_CHECK(false);
    } catch (const ruvia::validation_error& error) {
        const auto issues = error.info().validation_issues();
        RUVIA_CHECK_EQ(issues.size(), std::size_t{1});
        RUVIA_CHECK_EQ(issues[0].field(), std::string_view("f\"x"));
        RUVIA_CHECK_EQ(issues[0].message(), std::string_view("a\"b\\c"));
    }
}

RUVIA_TEST(validation_error_options_control_reported_error_info) {
    validator v;
    v.add("field", "required", "missing");

    try {
        v.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation",
        });
        RUVIA_CHECK(false);
    } catch (const ruvia::validation_error& error) {
        const auto info = error.info();
        RUVIA_CHECK_EQ(info.status(), ruvia::http_status::unprocessable_content);
        RUVIA_CHECK_EQ(info.code(), std::string_view("invalid_payload"));
        RUVIA_CHECK_EQ(info.message(), std::string_view("payload failed validation"));
        RUVIA_CHECK_EQ(info.validation_issues().size(), std::size_t{1});
    }
}

RUVIA_TEST(validation_error_throw_releases_validator_resource) {
    counting_memory_resource resource;
    const auto exception = capture_validation_exception(&resource);

    RUVIA_CHECK(exception != nullptr);
    RUVIA_CHECK(resource.allocation_count() > 0);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::validation_error& error) {
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("payload failed validation"));
        RUVIA_CHECK_EQ(error.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(error.issues()[0].field().size(), std::size_t{128});
        RUVIA_CHECK_EQ(error.issues()[0].message().size(), std::size_t{256});
    }
}

RUVIA_TEST(validation_error_survives_short_lived_arena_and_exception_ptr) {
    counting_memory_resource upstream;
    const auto exception = capture_validation_exception_from_short_lived_arena(&upstream);

    // The arena, its initial buffer, and the Validator have all gone away before
    // the exception is inspected. The exception owns process-lifetime copies.
    RUVIA_CHECK(exception != nullptr);
    RUVIA_CHECK_EQ(upstream.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocation_count(), upstream.deallocation_count());

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::validation_error& error) {
        RUVIA_CHECK_EQ(std::string_view(error.what()),
            std::string_view("request validation failed"));
        RUVIA_CHECK_EQ(error.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(error.issues()[0].field().size(), std::size_t{128});
        RUVIA_CHECK_EQ(error.issues()[0].message().size(), std::size_t{256});
    }
}

RUVIA_TEST(validation_error_copy_move_and_assignment_preserve_owned_data) {
    counting_memory_resource resource;
    const auto exception = capture_validation_exception(&resource);

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::validation_error& original) {
        ruvia::validation_error copied(original);
        ruvia::validation_error::issue_list_type empty_issues;
        ruvia::validation_error copy_assigned(empty_issues);
        copy_assigned = original;

        ruvia::validation_error moved(std::move(copied));
        ruvia::validation_error move_assigned(empty_issues);
        move_assigned = std::move(moved);

        for (const auto* error : {&copy_assigned, &move_assigned}) {
            RUVIA_CHECK_EQ(error->issues().size(), std::size_t{1});
            RUVIA_CHECK_EQ(error->issues()[0].field().size(), std::size_t{128});
            RUVIA_CHECK_EQ(error->issues()[0].message().size(), std::size_t{256});
            RUVIA_CHECK_EQ(std::string_view(error->what()),
                std::string_view("payload failed validation"));
        }
        RUVIA_CHECK_EQ(copied.issues().size(), std::size_t{0});
        RUVIA_CHECK_EQ(moved.issues().size(), std::size_t{0});
    }

    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(validator_throw_if_invalid_raises_on_issues) {
    validator ok;
    ok.throw_if_invalid();  // no issues -> no throw

    validator bad;
    std::optional<std::string> absent;
    bad.required(absent, "x");
    bool threw = false;
    try {
        bad.throw_if_invalid();
    } catch (const ruvia::validation_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(validation_diagnostic_text_preserves_complete_utf8_within_limit) {
    ruvia::validator validator;
    std::string message(ruvia::max_validation_text_bytes - 1, 'x');
    message += "中文";
    validator.add("field", "code", message);
    RUVIA_CHECK_EQ(validator.issues()[0].message().size(), ruvia::max_validation_text_bytes - 1);
    RUVIA_CHECK_EQ(validator.issues()[0].message(), std::string_view(message).substr(0, ruvia::max_validation_text_bytes - 1));
}
