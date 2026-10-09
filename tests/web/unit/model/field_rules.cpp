#include <memory_resource>
#include <string_view>

#include "ruvia/web/detail/model/rule/rules.h"
#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "test_harness.h"

RUVIA_MODEL(field_rule_profile,
    RUVIA_REQUIRED_FIELD(email, ruvia::string, RUVIA_EMAIL("email format is invalid")),
    RUVIA_OPTIONAL_FIELD(age, ruvia::uint32, RUVIA_MIN(0, "age is too small"),
        RUVIA_MAX(130, "age is too large")));

RUVIA_MODEL(field_rule_user,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(2, "name is too short")),
    RUVIA_REQUIRED_FIELD(profile, field_rule_profile),
    RUVIA_REQUIRED_FIELD(roles, ruvia::array<field_rule_profile>, RUVIA_MIN(1, "too few roles")));

RUVIA_MODEL(field_rule_matrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::array<field_rule_profile>>));

RUVIA_MODEL(field_rule_boxed_matrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::boxed_array<ruvia::boxed_array<field_rule_profile>>));

RUVIA_MODEL(field_rule_mixed_matrix,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::boxed_array<ruvia::array<field_rule_profile>>>));

RUVIA_MODEL(field_rule_wide_integer,
    RUVIA_REQUIRED_FIELD(value, ruvia::uint64,
        RUVIA_MIN(9007199254740993ULL, "below exact minimum"),
        RUVIA_MAX(9007199254740995ULL, "above exact maximum")));

RUVIA_MODEL(field_rule_decimal,
    RUVIA_REQUIRED_FIELD(value, ruvia::double_value, RUVIA_MIN(0.5, "below minimum")));

namespace {

void check(const field_rule_user& model, ruvia::validator& validator_value) {
    ruvia::detail::model_validation_access::validate_model(model, validator_value);
}

template <typename model_t_type>
void check_array_validation(auto& ruvia_ctx, std::string_view json, std::string_view expected_path,
    std::string_view expected_code, bool structure_valid) {
    auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<model_t_type>(
        json, std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK_EQ(ruvia::detail::model_validation_access::structure_valid(*parsed_value), structure_valid);
    ruvia::validator validator;
    ruvia::detail::model_validation_access::validate_model(*parsed_value, validator);
    if (expected_code.empty()) {
        RUVIA_CHECK(validator.ok());
    } else {
        RUVIA_CHECK_EQ(validator.issues().size(), 1U);
        if (!validator.issues().empty()) {
            RUVIA_CHECK_EQ(validator.issues().front().field(), expected_path);
            RUVIA_CHECK_EQ(validator.issues().front().code(), expected_code);
        }
    }
}

template <typename model_t_type>
void check_matrix_validation(auto& ruvia_ctx) {
    check_array_validation<model_t_type>(ruvia_ctx, R"({"items":[[],[{"email":"a@b.co"}]]})", {}, {}, true);
    check_array_validation<model_t_type>(ruvia_ctx, R"({"items":[[],[{"email":"bad"}]]})", "items[1][0].email", "email", true);
    check_array_validation<model_t_type>(ruvia_ctx, R"({"items":[[],[{}]]})", "items[1][0].email", "required", false);
    check_array_validation<model_t_type>(ruvia_ctx, R"({"items":[[],[{"email":42}]]})", "items[1][0].email", "invalid_type", false);
    check_array_validation<model_t_type>(ruvia_ctx, R"({"items":[[],[{"email":"a@b.co","email":"c@d.co"}]]})", "items[1][0].email", "duplicate", false);
}

}  // namespace

RUVIA_TEST(model_field_rules_validate_multidimensional_arrays) {
    check_matrix_validation<field_rule_matrix>(ruvia_ctx);
    check_matrix_validation<field_rule_boxed_matrix>(ruvia_ctx);
    check_array_validation<field_rule_mixed_matrix>(ruvia_ctx, R"({"items":[[[{"email":"bad"}]]]})",
        "items[0][0][0].email", "email", true);
    check_array_validation<field_rule_mixed_matrix>(ruvia_ctx, R"({"items":[[[{}]]]})",
        "items[0][0][0].email", "required", false);
    check_array_validation<field_rule_mixed_matrix>(ruvia_ctx, R"({"items":[[[{"email":"a@b.co"}]]]})",
        {}, {}, true);
}

RUVIA_TEST(model_field_rules_keep_exact_integer_and_decimal_bounds) {
    field_rule_wide_integer integer;
    ruvia::validator below;
    integer.set<"value">(9007199254740992ULL);
    ruvia::detail::model_validation_access::validate_model(integer, below);
    RUVIA_CHECK_EQ(below.issues().size(), 1U);
    RUVIA_CHECK_EQ(below.issues().front().code(), std::string_view("too_small"));

    ruvia::validator within;
    integer.set<"value">(9007199254740993ULL);
    ruvia::detail::model_validation_access::validate_model(integer, within);
    RUVIA_CHECK(within.ok());

    ruvia::validator above;
    integer.set<"value">(9007199254740996ULL);
    ruvia::detail::model_validation_access::validate_model(integer, above);
    RUVIA_CHECK_EQ(above.issues().size(), 1U);
    RUVIA_CHECK_EQ(above.issues().front().code(), std::string_view("too_big"));

    field_rule_decimal decimal;
    decimal.set<"value">(0.25);
    ruvia::validator decimal_validator;
    ruvia::detail::model_validation_access::validate_model(decimal, decimal_validator);
    RUVIA_CHECK_EQ(decimal_validator.issues().size(), 1U);
}

RUVIA_TEST(model_field_rules_accept_valid_nested_payload) {
    auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<field_rule_user>(
        R"({"name":"Al","profile":{"email":"a@b.co"},"roles":[{"email":"c@d.co"}]})",
        std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    ruvia::validator validator;
    check(*parsed_value, validator);
    RUVIA_CHECK(validator.ok());
}

RUVIA_TEST(model_field_rules_reject_short_name_and_nested_email) {
    auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<field_rule_user>(
        R"({"name":"A","profile":{"email":"bad"},"roles":[]})", std::pmr::get_default_resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    ruvia::validator validator;
    check(*parsed_value, validator);
    RUVIA_CHECK(!validator.ok());
    bool saw_name = false;
    bool saw_email = false;
    bool saw_roles = false;
    for (const auto& issue : validator.issues()) {
        if (issue.field() == "name" && issue.code() == "too_small") {
            saw_name = true;
        }
        if (issue.field() == "profile.email" && issue.code() == "email") {
            saw_email = true;
        }
        if (issue.field() == "roles" && issue.code() == "too_small") {
            saw_roles = true;
        }
    }
    RUVIA_CHECK(saw_name);
    RUVIA_CHECK(saw_email);
    RUVIA_CHECK(saw_roles);
}
