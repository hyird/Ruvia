#include <cstddef>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/Controller.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"

#include "test_harness.h"

namespace {

using ruvia::Validator;

RUVIA_REQUEST_MODEL(RequiredOptionalModel, RUVIA_REQUIRED_FIELD(requiredValue, ruvia::String),
    RUVIA_OPTIONAL_FIELD(optionalValue, ruvia::String));

RUVIA_REQUEST_MODEL(RequiredRulesModel, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(name, ruvia::String), RUVIA_REQUIRED_FIELD(age, ruvia::UInt32));

class RequiredRulesValidator final {
public:
    RUVIA_VALIDATE_JSON(RequiredRulesModel,
        RUVIA_RULE(id, RUVIA_REQUIRED("id is required"), RUVIA_MIN(1, "id is too short"),
            RUVIA_MAX(64, "id is too long")),
        RUVIA_RULE(name, RUVIA_REQUIRED("name is required"), RUVIA_MIN(1, "name is too short"),
            RUVIA_MAX(120, "name is too long")),
        RUVIA_RULE(age, RUVIA_REQUIRED("age is required"), RUVIA_MAX(130, "age is too large")))
};

RUVIA_REQUEST_MODEL(OptionalRulesModel, RUVIA_OPTIONAL_FIELD(value, ruvia::String));

class OptionalRulesValidator final {
public:
    RUVIA_VALIDATE_JSON(OptionalRulesModel,
        RUVIA_RULE(value, RUVIA_REQUIRED("value is required"), RUVIA_MIN(1, "value is empty")))
};

}  // namespace

RUVIA_TEST(model_rules_validate_required_values_and_preserve_parse_errors) {
    RequiredRulesValidator rules;

    const auto valid = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRulesModel>(
        R"({"id":"u-1","name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(valid.has_value());
    if (!valid) {
        return;
    }
    Validator validValidator;
    rules.validate(*valid, validValidator);
    RUVIA_CHECK(validValidator.ok());

    const auto invalidType =
        ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRulesModel>(
            R"({"id":42,"name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(invalidType.has_value());
    if (invalidType) {
        Validator validator;
        rules.validate(*invalidType, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("invalid_type"));
    }

    const auto duplicate =
        ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRulesModel>(
            R"({"id":"first","id":"second","name":"Alice","age":32})",
            std::pmr::get_default_resource());
    RUVIA_CHECK(duplicate.has_value());
    if (duplicate) {
        Validator validator;
        rules.validate(*duplicate, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("duplicate"));
    }

    const auto missing =
        ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRulesModel>(
            R"({"name":"Alice","age":32})", std::pmr::get_default_resource());
    RUVIA_CHECK(missing.has_value());
    if (missing) {
        Validator validator;
        rules.validate(*missing, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("id"));
        RUVIA_CHECK_EQ(validator.issues()[0].code(), std::string_view("required"));
    }
}

RUVIA_TEST(request_model_required_and_optional_fields_are_structural) {
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredOptionalModel>(
        "{}", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }

    Validator validator;
    ruvia::detail::ModelValidationAccess::validateStructure(*parsed, {}, validator);
    RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("requiredValue"));
    RUVIA_CHECK(!ruvia::fromJson<RequiredOptionalModel>("{}").has_value());

    RUVIA_CHECK(
        !ruvia::fromForm<RequiredOptionalModel>("", {.resource = std::pmr::get_default_resource()})
            .has_value());
    auto partialForm =
        ruvia::detail::ModelParseAccess::parseFormBorrowedPartial<RequiredOptionalModel>(
            "", std::pmr::get_default_resource());
    RUVIA_CHECK(partialForm.has_value());
    if (partialForm) {
        Validator formValidator;
        ruvia::detail::ModelValidationAccess::validateStructure(*partialForm, {}, formValidator);
        RUVIA_CHECK_EQ(formValidator.issues().size(), std::size_t{1});
        RUVIA_CHECK_EQ(formValidator.issues()[0].field(), std::string_view("requiredValue"));
    }
}

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

    v.minLength(value, "f", 5);  // 3 < 5 -> too_small
    v.maxLength(value, "f", 2);  // 3 > 2 -> too_big
    // An absent value is never checked.
    std::optional<std::string> absent;
    v.minLength(absent, "g", 100);

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("too_big"));
}

RUVIA_TEST(validator_range_and_one_of) {
    Validator v;
    std::optional<int> n = 5;
    v.range(n, "n", 1, 10);  // in range, ok
    RUVIA_CHECK(v.ok());
    v.range(n, "n", 6, 10);  // 5 < 6 -> too_small

    std::optional<std::string> s = std::string("b");
    v.oneOf(s, "s", {"a", "b", "c"});  // allowed, ok
    v.oneOf(s, "s", {"x", "y"});       // not allowed -> one_of

    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("one_of"));
}

RUVIA_TEST(validator_range_upper_bound_inclusive_and_absent_skips) {
    Validator v;
    // The upper bound is enforced independently of the lower bound.
    std::optional<int> high = 5;
    v.range(high, "high", 1, 3);  // 5 > 3 -> too_big
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{1});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_big"));
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
    v.range(inRange, "d", 0.0, 1.0);  // 0.0 <= 0.5 <= 1.0, ok
    RUVIA_CHECK(v.ok());
    std::optional<double> low = -0.1;
    v.range(low, "d", 0.0, 1.0);  // -0.1 < 0.0 -> too_small
    std::optional<double> high = 1.1;
    v.range(high, "d", 0.0, 1.0);  // 1.1 > 1.0 -> too_big
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(v.issues()[0].code(), std::string_view("too_small"));
    RUVIA_CHECK_EQ(v.issues()[1].code(), std::string_view("too_big"));

    // Both floating bounds are inclusive: a value exactly at min or max is accepted.
    std::optional<double> atMin = 0.0;
    std::optional<double> atMax = 1.0;
    v.range(atMin, "d", 0.0, 1.0);
    v.range(atMax, "d", 0.0, 1.0);
    RUVIA_CHECK_EQ(v.issues().size(), std::size_t{2});  // unchanged
}

RUVIA_TEST(validation_error_exposes_typed_issues) {
    Validator v;
    std::optional<std::string> absent;
    v.required(absent, "email", "email is required");
    std::optional<std::string> shortName = std::string("a");
    v.minLength(shortName, "name", 3, "too short");

    try {
        v.throwIfInvalid();
        RUVIA_CHECK(false);  // must have thrown
    } catch (const ruvia::ValidationError& error) {
        const auto issues = error.info().validationIssues();
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
    Validator v;
    v.add("f\"x", "code", "a\"b\\c");
    try {
        v.throwIfInvalid();
        RUVIA_CHECK(false);
    } catch (const ruvia::ValidationError& error) {
        const auto issues = error.info().validationIssues();
        RUVIA_CHECK_EQ(issues.size(), std::size_t{1});
        RUVIA_CHECK_EQ(issues[0].field(), std::string_view("f\"x"));
        RUVIA_CHECK_EQ(issues[0].message(), std::string_view("a\"b\\c"));
    }
}

RUVIA_TEST(validation_error_options_control_reported_error_info) {
    Validator v;
    v.add("field", "required", "missing");

    try {
        v.throwIfInvalid({
            .status = ruvia::http_status::kUnprocessableContent,
            .code = "invalid_payload",
            .message = "payload failed validation",
        });
        RUVIA_CHECK(false);
    } catch (const ruvia::ValidationError& error) {
        const auto info = error.info();
        RUVIA_CHECK_EQ(info.status(), ruvia::http_status::kUnprocessableContent);
        RUVIA_CHECK_EQ(info.code(), std::string_view("invalid_payload"));
        RUVIA_CHECK_EQ(info.message(), std::string_view("payload failed validation"));
        RUVIA_CHECK_EQ(info.validationIssues().size(), std::size_t{1});
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

RUVIA_TEST(validated_model_bindings_are_nested_scoped_borrows) {
    ruvia::detail::RequestBindings values;
    int number = 42;
    {
        auto numberBinding = values.bindValidated(number);
        RUVIA_CHECK_EQ(values.getValidated<int>(), 42);

        {
            std::string text = "nested";
            auto textBinding = values.bindValidated(text);
            RUVIA_CHECK_EQ(values.getValidated<std::string>(), std::string("nested"));
            RUVIA_CHECK_EQ(values.getValidated<int>(), 42);
        }

        // The inner borrow must unbind on its own and leave the outer one live.
        bool nestedReleased = false;
        try {
            (void)values.getValidated<std::string>();
        } catch (const std::logic_error&) {
            nestedReleased = true;
        }
        RUVIA_CHECK(nestedReleased);
        RUVIA_CHECK_EQ(values.getValidated<int>(), 42);
    }

    bool missingRejected = false;
    try {
        (void)values.getValidated<int>();
    } catch (const std::logic_error&) {
        missingRejected = true;
    }
    RUVIA_CHECK(missingRejected);
}

RUVIA_TEST(validated_json_binding_exposes_typed_value_and_exact_raw_body) {
    ruvia::detail::RequestBindings values;
    int number = 42;
    constexpr std::string_view raw = R"( {"value":42} )";
    auto binding = values.bindValidated(number, raw);
    const auto json = values.getValidatedJson<int>();
    RUVIA_CHECK_EQ(json.value(), 42);
    RUVIA_CHECK_EQ(json.raw(), raw);
}

RUVIA_TEST(validated_model_binding_unwinds_on_exception) {
    ruvia::detail::RequestBindings values;
    try {
        int number = 7;
        auto binding = values.bindValidated(number);
        throw std::runtime_error("leave validation scope");
    } catch (const std::runtime_error&) {
    }

    bool missingRejected = false;
    try {
        (void)values.getValidated<int>();
    } catch (const std::logic_error&) {
        missingRejected = true;
    }
    RUVIA_CHECK(missingRejected);
}

// Request state shares the intrusive stack with validated models but must never
// answer their lookups: validated<T>() promises "a validator checked this", and
// hand-bound state impersonating it would silently void that promise.

RUVIA_TEST(request_state_and_validated_model_do_not_answer_each_other) {
    ruvia::detail::RequestBindings values;
    int number = 42;

    auto stateBinding = values.bindState(number);
    RUVIA_CHECK_EQ(values.getState<int>(), 42);

    // Bound as state, so the validated lookup must not find it.
    bool validatedRejected = false;
    try {
        (void)values.getValidated<int>();
    } catch (const std::logic_error&) {
        validatedRejected = true;
    }
    RUVIA_CHECK(validatedRejected);

    // ...and symmetrically for a validated binding of the same type.
    int validatedNumber = 7;
    auto validatedBinding = values.bindValidated(validatedNumber);
    RUVIA_CHECK_EQ(values.getValidated<int>(), 7);
    RUVIA_CHECK_EQ(values.getState<int>(), 42);
}

RUVIA_TEST(request_state_try_lookup_reports_absence_without_throwing) {
    ruvia::detail::RequestBindings values;
    RUVIA_CHECK(values.tryGetState<int>() == nullptr);

    int number = 5;
    {
        auto binding = values.bindState(number);
        const auto* found = values.tryGetState<int>();
        RUVIA_CHECK(found != nullptr);
        RUVIA_CHECK_EQ(*found, 5);
        // Bound by address, never copied.
        RUVIA_CHECK(found == &number);
    }
    RUVIA_CHECK(values.tryGetState<int>() == nullptr);
}

RUVIA_TEST(request_state_nested_binding_shadows_then_restores) {
    ruvia::detail::RequestBindings values;
    int outer = 1;
    auto outerBinding = values.bindState(outer);
    {
        int inner = 2;
        auto innerBinding = values.bindState(inner);
        RUVIA_CHECK_EQ(values.getState<int>(), 2);
    }
    RUVIA_CHECK_EQ(values.getState<int>(), 1);
}

RUVIA_TEST(model_rules_enforce_bounds_on_required_values) {
    const auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRulesModel>(
        R"({"id":"","name":"Alice","age":131})", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    Validator validator;
    RequiredRulesValidator{}.validate(*parsed, validator);
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
        const auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<OptionalRulesModel>(
            body, std::pmr::get_default_resource());
        RUVIA_CHECK(parsed.has_value());
        if (!parsed) {
            continue;
        }
        Validator validator;
        OptionalRulesValidator{}.validate(*parsed, validator);
        RUVIA_CHECK_EQ(validator.issues().size(), expected.empty() ? std::size_t{0} : std::size_t{1});
        if (!expected.empty() && !validator.issues().empty()) {
            RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("value"));
            RUVIA_CHECK_EQ(validator.issues()[0].code(), expected);
        }
    }
}
