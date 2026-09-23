#include <memory_resource>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/model/rule/Rules.h"

#include "test_harness.h"

RUVIA_MODEL(FieldRuleProfile,
    RUVIA_REQUIRED_FIELD(email, ruvia::String, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32, RUVIA_MIN(0, "age is too small"),
        RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(FieldRuleUser,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "name is too short")),
    RUVIA_REQUIRED_FIELD(profile, FieldRuleProfile),
    RUVIA_REQUIRED_FIELD(roles, ruvia::Array<FieldRuleProfile>, RUVIA_MIN(1, "too few roles")));

RUVIA_MODEL(FieldRuleMatrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<ruvia::Array<FieldRuleProfile>>));

RUVIA_MODEL(FieldRuleBoxedMatrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::BoxedArray<ruvia::BoxedArray<FieldRuleProfile>>));

RUVIA_MODEL(FieldRuleMixedMatrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<ruvia::BoxedArray<ruvia::Array<FieldRuleProfile>>>));

RUVIA_MODEL(FieldRuleWideInteger,
    RUVIA_REQUIRED_FIELD(value, ruvia::UInt64,
        RUVIA_MIN(9007199254740993ULL, "below exact minimum"),
        RUVIA_MAX(9007199254740995ULL, "above exact maximum")));

RUVIA_MODEL(FieldRuleDecimal,
    RUVIA_REQUIRED_FIELD(value, ruvia::Double, RUVIA_MIN(0.5, "below minimum")));

namespace {

void check(const FieldRuleUser& model, ruvia::Validator& validator) {
    ruvia::detail::ModelValidationAccess::validateModel(model, validator);
}

template <typename ModelT>
void checkArrayValidation(auto& ruvia_ctx, std::string_view json, std::string_view expectedPath,
    std::string_view expectedCode, bool structureValid) {
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<ModelT>(
        json, std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK_EQ(ruvia::detail::ModelValidationAccess::structureValid(*parsed), structureValid);
    ruvia::Validator validator;
    ruvia::detail::ModelValidationAccess::validateModel(*parsed, validator);
    if (expectedCode.empty()) {
        RUVIA_CHECK(validator.ok());
    } else {
        RUVIA_CHECK_EQ(validator.issues().size(), 1U);
        if (!validator.issues().empty()) {
            RUVIA_CHECK_EQ(validator.issues().front().field(), expectedPath);
            RUVIA_CHECK_EQ(validator.issues().front().code(), expectedCode);
        }
    }
}

template <typename ModelT>
void checkMatrixValidation(auto& ruvia_ctx) {
    checkArrayValidation<ModelT>(ruvia_ctx, R"({"items":[[],[{"email":"a@b.co"}]]})", {}, {}, true);
    checkArrayValidation<ModelT>(ruvia_ctx, R"({"items":[[],[{"email":"bad"}]]})", "items[1][0].email", "email", true);
    checkArrayValidation<ModelT>(ruvia_ctx, R"({"items":[[],[{}]]})", "items[1][0].email", "required", false);
    checkArrayValidation<ModelT>(ruvia_ctx, R"({"items":[[],[{"email":42}]]})", "items[1][0].email", "invalid_type", false);
    checkArrayValidation<ModelT>(ruvia_ctx, R"({"items":[[],[{"email":"a@b.co","email":"c@d.co"}]]})", "items[1][0].email", "duplicate", false);
}

}  // namespace

RUVIA_TEST(model_field_rules_validate_multidimensional_arrays) {
    checkMatrixValidation<FieldRuleMatrix>(ruvia_ctx);
    checkMatrixValidation<FieldRuleBoxedMatrix>(ruvia_ctx);
    checkArrayValidation<FieldRuleMixedMatrix>(ruvia_ctx, R"({"items":[[[{"email":"bad"}]]]})",
        "items[0][0][0].email", "email", true);
    checkArrayValidation<FieldRuleMixedMatrix>(ruvia_ctx, R"({"items":[[[{}]]]})",
        "items[0][0][0].email", "required", false);
    checkArrayValidation<FieldRuleMixedMatrix>(ruvia_ctx, R"({"items":[[[{"email":"a@b.co"}]]]})",
        {}, {}, true);
}

RUVIA_TEST(model_field_rules_keep_exact_integer_and_decimal_bounds) {
    FieldRuleWideInteger integer;
    ruvia::Validator below;
    integer.set<"value">(9007199254740992ULL);
    ruvia::detail::ModelValidationAccess::validateModel(integer, below);
    RUVIA_CHECK_EQ(below.issues().size(), 1U);
    RUVIA_CHECK_EQ(below.issues().front().code(), std::string_view("too_small"));

    ruvia::Validator within;
    integer.set<"value">(9007199254740993ULL);
    ruvia::detail::ModelValidationAccess::validateModel(integer, within);
    RUVIA_CHECK(within.ok());

    ruvia::Validator above;
    integer.set<"value">(9007199254740996ULL);
    ruvia::detail::ModelValidationAccess::validateModel(integer, above);
    RUVIA_CHECK_EQ(above.issues().size(), 1U);
    RUVIA_CHECK_EQ(above.issues().front().code(), std::string_view("too_big"));

    FieldRuleDecimal decimal;
    decimal.set<"value">(0.25);
    ruvia::Validator decimalValidator;
    ruvia::detail::ModelValidationAccess::validateModel(decimal, decimalValidator);
    RUVIA_CHECK_EQ(decimalValidator.issues().size(), 1U);
}

RUVIA_TEST(model_field_rules_accept_valid_nested_payload) {
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<FieldRuleUser>(
        R"({"name":"Al","profile":{"email":"a@b.co"},"roles":[{"email":"c@d.co"}]})",
        std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    ruvia::Validator validator;
    check(*parsed, validator);
    RUVIA_CHECK(validator.ok());
}

RUVIA_TEST(model_field_rules_reject_short_name_and_nested_email) {
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<FieldRuleUser>(
        R"({"name":"A","profile":{"email":"bad"},"roles":[]})", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    ruvia::Validator validator;
    check(*parsed, validator);
    RUVIA_CHECK(!validator.ok());
    bool sawName = false;
    bool sawEmail = false;
    bool sawRoles = false;
    for (const auto& issue : validator.issues()) {
        if (issue.field() == "name" && issue.code() == "too_small") {
            sawName = true;
        }
        if (issue.field() == "profile.email" && issue.code() == "email") {
            sawEmail = true;
        }
        if (issue.field() == "roles" && issue.code() == "too_small") {
            sawRoles = true;
        }
    }
    RUVIA_CHECK(sawName);
    RUVIA_CHECK(sawEmail);
    RUVIA_CHECK(sawRoles);
}
