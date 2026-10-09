#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/error.h"
#include "ruvia/web/model.h"
#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::validator;
using ruvia::test::counting_memory_resource;

RUVIA_MODEL(required_optional_model, RUVIA_REQUIRED_FIELD_NAME("requiredValue", required_value, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("optionalValue", optional_value, ruvia::string));

[[nodiscard]] std::exception_ptr capture_validation_exception(
    std::pmr::memory_resource* resource, bool move_validator) {
    validator validator_value({.resource_ = resource});
    validator_value.add(std::string(128, 'f'), "required", std::string(256, 'm'));
    try {
        if (move_validator) {
            std::move(validator_value).throw_if_invalid({
                .status_ = ruvia::http_status::unprocessable_content,
                .code_ = "invalid_payload",
                .message_ = "payload failed validation",
            });
        } else {
            validator_value.throw_if_invalid({
                .status_ = ruvia::http_status::unprocessable_content,
                .code_ = "invalid_payload",
                .message_ = "payload failed validation",
            });
        }
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

RUVIA_TEST(validation_error_lvalue_and_rvalue_throws_release_validator_resource) {
    for (const bool move_validator : {false, true}) {
        counting_memory_resource resource;
        const auto exception = capture_validation_exception(&resource, move_validator);

        RUVIA_CHECK(exception != nullptr);
        RUVIA_CHECK(resource.allocation_count() > 0);
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        RUVIA_CHECK_EQ(resource.allocation_count(), resource.deallocation_count());

        try {
            std::rethrow_exception(exception);
        } catch (const ruvia::validation_error& error) {
            RUVIA_CHECK_EQ(std::string_view(error.what()),
                std::string_view("payload failed validation"));
            RUVIA_CHECK_EQ(error.issues().size(), std::size_t{1});
            RUVIA_CHECK_EQ(error.issues()[0].field().size(), std::size_t{128});
            RUVIA_CHECK_EQ(error.issues()[0].message().size(), std::size_t{256});
        }
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
    const auto exception = capture_validation_exception(&resource, false);

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
