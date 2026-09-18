#include <memory_resource>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"
#include "ruvia/web/detail/model/rule/Rules.h"

#include "test_harness.h"

RUVIA_REQUEST_MODEL(FieldRuleProfile,
    RUVIA_REQUIRED_FIELD(email, ruvia::String, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(age, ruvia::UInt32, RUVIA_MIN(0, "age is too small"),
        RUVIA_MAX(130, "age is too large")));

RUVIA_REQUEST_MODEL(FieldRuleUser,
    RUVIA_REQUIRED_FIELD(name, ruvia::String, RUVIA_MIN(2, "name is too short")),
    RUVIA_REQUIRED_FIELD(profile, FieldRuleProfile),
    RUVIA_REQUIRED_FIELD(roles, ruvia::Array<FieldRuleProfile>, RUVIA_MIN(1, "too few roles")));

namespace {

void check(const FieldRuleUser& model, ruvia::Validator& validator) {
    ruvia::detail::ModelValidationAccess::validateModel(model, validator);
}

}  // namespace

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
