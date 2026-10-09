#include <array>
#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/controller.h"
#include "ruvia/web/detail/http/context/request_bindings.h"
#include "ruvia/web/error.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::validator;
using ruvia::test::counting_memory_resource;

RUVIA_MODEL(required_optional_model, RUVIA_REQUIRED_FIELD_NAME("requiredValue", required_value, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("optionalValue", optional_value, ruvia::string));

RUVIA_MODEL(required_rules_model,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(1, "id is too short"),
        RUVIA_MAX(64, "id is too long")),
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(1, "name is too short"),
        RUVIA_MAX(120, "name is too long")),
    RUVIA_REQUIRED_FIELD(age, ruvia::uint32, RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(optional_rules_model,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_MIN(1, "value is empty")));

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

RUVIA_TEST(model_rules_validate_required_values_and_preserve_parse_errors) {
    const auto valid = ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_rules_model>(
        R"({"id":"u-1","name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(valid.has_value());
    if (!valid) {
        return;
    }
    validator valid_validator;
    ruvia::detail::model_validation_access::validate_model(*valid, valid_validator);
    RUVIA_CHECK(valid_validator.ok());

    const auto invalid_type =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_rules_model>(
            R"({"id":42,"name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(invalid_type.has_value());
    if (invalid_type) {
        validator validator;
        ruvia::detail::model_validation_access::validate_model(*invalid_type, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("invalid_type"));
    }

    const auto duplicate =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_rules_model>(
            R"({"id":"first","id":"second","name":"Alice","age":32})",
            std::pmr::get_default_resource());
    RUVIA_CHECK(duplicate.has_value());
    if (duplicate) {
        validator validator;
        ruvia::detail::model_validation_access::validate_model(*duplicate, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("duplicate"));
    }

    const auto missing =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_rules_model>(
            R"({"name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(missing.has_value());
    if (missing) {
        validator validator;
        ruvia::detail::model_validation_access::validate_model(*missing, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("required"));
    }
}

RUVIA_TEST(request_model_required_and_optional_fields_are_structural) {
    auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_optional_model>(
        "{}", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }

    validator validator;
    ruvia::detail::model_validation_access::validate_structure(*parsed_value, {}, validator);
    RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("requiredValue"));
    RUVIA_CHECK(!ruvia::from_json<required_optional_model>("{}").has_value());

    RUVIA_CHECK(
        !ruvia::from_form<required_optional_model>("", {.resource_ = std::pmr::get_default_resource()})
            .has_value());
    auto partial_form =
        ruvia::detail::model_parse_access::parse_form_borrowed_partial<required_optional_model>(
            "", std::pmr::get_default_resource());
    RUVIA_CHECK(partial_form.has_value());
    if (partial_form) {
        ruvia::validator form_validator;
        ruvia::detail::model_validation_access::validate_structure(*partial_form, {}, form_validator);
        RUVIA_CHECK_EQ(form_validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(form_validator.issues()[0].field(), std::string_view("requiredValue"));
    }
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
            RUVIA_CHECK_EQ(error.issues().get_allocator().resource(),
                ruvia::detail::process_resource());
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

RUVIA_TEST(validated_model_bindings_are_nested_scoped_borrows) {
    ruvia::detail::request_bindings values;
    int number = 42;
    {
        auto number_binding = values.bind_validated(number);
        RUVIA_CHECK_EQ(values.get_validated<int>(), 42);

        {
            std::string text = "nested";
            auto text_binding = values.bind_validated(text);
            RUVIA_CHECK_EQ(values.get_validated<std::string>(), std::string("nested"));
            RUVIA_CHECK_EQ(values.get_validated<int>(), 42);
        }

        // The inner borrow must unbind on its own and leave the outer one live.
        bool nested_released = false;
        try {
            (void)values.get_validated<std::string>();
        } catch (const std::logic_error&) {
            nested_released = true;
        }
        RUVIA_CHECK(nested_released);
        RUVIA_CHECK_EQ(values.get_validated<int>(), 42);
    }

    bool missing_rejected = false;
    try {
        (void)values.get_validated<int>();
    } catch (const std::logic_error&) {
        missing_rejected = true;
    }
    RUVIA_CHECK(missing_rejected);
}

RUVIA_TEST(validated_json_binding_exposes_typed_value_and_exact_raw_body) {
    ruvia::detail::request_bindings values;
    int number = 42;
    constexpr std::string_view raw = R"( {"value":42} )";
    auto binding = values.bind_validated(number, raw);
    const auto json = values.get_validated_json<int>();
    RUVIA_CHECK_EQ(json.value(), 42);
    RUVIA_CHECK_EQ(json.raw(), raw);
}

RUVIA_TEST(validated_model_binding_unwinds_on_exception) {
    ruvia::detail::request_bindings values;
    try {
        int number = 7;
        auto binding = values.bind_validated(number);
        throw std::runtime_error("leave validation scope");
    } catch (const std::runtime_error&) {
    }

    bool missing_rejected = false;
    try {
        (void)values.get_validated<int>();
    } catch (const std::logic_error&) {
        missing_rejected = true;
    }
    RUVIA_CHECK(missing_rejected);
}

// Request state shares the intrusive stack with validated models but must never
// answer their lookups: validated<T>() promises "a validator checked this", and
// hand-bound state impersonating it would silently void that promise.

RUVIA_TEST(request_state_and_validated_model_do_not_answer_each_other) {
    ruvia::detail::request_bindings values;
    int number = 42;

    auto state_binding = values.bind_state(number);
    RUVIA_CHECK_EQ(values.get_state<int>(), 42);

    // Bound as state, so the validated lookup must not find it.
    bool validated_rejected = false;
    try {
        (void)values.get_validated<int>();
    } catch (const std::logic_error&) {
        validated_rejected = true;
    }
    RUVIA_CHECK(validated_rejected);

    // ...and symmetrically for a validated binding of the same type.
    int validated_number = 7;
    auto validated_binding = values.bind_validated(validated_number);
    RUVIA_CHECK_EQ(values.get_validated<int>(), 7);
    RUVIA_CHECK_EQ(values.get_state<int>(), 42);
}

RUVIA_TEST(request_state_try_lookup_reports_absence_without_throwing) {
    ruvia::detail::request_bindings values;
    RUVIA_CHECK(values.try_get_state<int>() == nullptr);

    int number = 5;
    {
        auto binding = values.bind_state(number);
        const auto* found = values.try_get_state<int>();
        RUVIA_CHECK(found != nullptr);
        RUVIA_CHECK_EQ(*found, 5);
        // Bound by address, never copied.
        RUVIA_CHECK(found == &number);
    }
    RUVIA_CHECK(values.try_get_state<int>() == nullptr);
}

RUVIA_TEST(request_state_nested_binding_shadows_then_restores) {
    ruvia::detail::request_bindings values;
    int outer = 1;
    auto outer_binding = values.bind_state(outer);
    {
        int inner = 2;
        auto inner_binding = values.bind_state(inner);
        RUVIA_CHECK_EQ(values.get_state<int>(), 2);
    }
    RUVIA_CHECK_EQ(values.get_state<int>(), 1);
}

RUVIA_TEST(model_rules_enforce_bounds_on_required_values) {
    const auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<required_rules_model>(
        R"({"id":"","name":"Alice","age":131})", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    validator validator;
    ruvia::detail::model_validation_access::validate_model(*parsed_value, validator);
    RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
    RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(validator.issues()[1].field(), std::string_view("age"));
    RUVIA_CHECK_EQ(validator.issues()[1].code(), std::string_view("too_big"));
}

RUVIA_TEST(model_rules_require_optional_fields_without_duplicate_parse_errors) {
    const std::pair<std::string_view, std::string_view> cases[] = {
        {R"({})", "required"},
        {R"({"value":42})", "invalid_type"},
        {R"({"value":null})", "invalid_type"},
        {R"({"value":"first","value":"second"})", "duplicate"},
        {R"({"value":""})", "too_small"},
        {R"({"value":"valid"})", ""},
    };
    for (const auto& [body, expected] : cases) {
        const auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<optional_rules_model>(
            body, std::pmr::get_default_resource());
        RUVIA_CHECK(parsed_value.has_value());
        if (!parsed_value) {
            continue;
        }
        validator validator;
        ruvia::detail::model_validation_access::validate_model(*parsed_value, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), expected.empty() ? std::size_t{0} : std::size_t{1});
        if (!expected.empty() && !validator.issues().empty()) {
            RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("value"));
            RUVIA_CHECK_EQ(validator.issues()[0].code(), expected);
        }
    }
}

namespace {
std::size_t bounded_rule_calls = 0;
bool bounded_invalid_rule(const ruvia::string&) {
    ++bounded_rule_calls;
    return false;
}
RUVIA_MODEL(bounded_validation_item,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_CUSTOM("invalid value", bounded_invalid_rule)));
RUVIA_MODEL(bounded_validation_model,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<bounded_validation_item>));
}  // namespace

RUVIA_TEST(validation_diagnostics_stop_at_the_document_limit) {
    bounded_validation_model model;
    auto& items = model.ensure<"items">();
    for (std::size_t index = 0; index < 4 * ruvia::max_validation_issues; ++index) {
        items.emplace_back().set<"value">("invalid");
    }
    bounded_rule_calls = 0;
    ruvia::validator validator;
    ruvia::detail::model_validation_access::validate_model(model, validator);
    RUVIA_CHECK_EQ(validator.issues().size(), ruvia::max_validation_issues);
    RUVIA_CHECK_EQ(bounded_rule_calls, ruvia::max_validation_issues);
    RUVIA_CHECK(validator.full());
    RUVIA_CHECK_EQ(validator.issues().front().field(), std::string_view("items[0].value"));
    try {
        validator.throw_if_invalid();
        RUVIA_CHECK(false);
    } catch (const ruvia::validation_error& error) {
        RUVIA_CHECK_EQ(error.issues().size(), ruvia::max_validation_issues);
    }
}

RUVIA_TEST(validation_diagnostic_text_preserves_complete_utf8_within_limit) {
    ruvia::validator validator;
    std::string message(ruvia::max_validation_text_bytes - 1, 'x');
    message += "中文";
    validator.add("field", "code", message);
    RUVIA_CHECK_EQ(validator.issues()[0].message().size(), ruvia::max_validation_text_bytes - 1);
    RUVIA_CHECK_EQ(validator.issues()[0].message(), std::string_view(message).substr(0, ruvia::max_validation_text_bytes - 1));
}
