#pragma once

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/RuleValidation.h"

namespace ruvia::detail {

struct ModelValidationAccess final {
    template <FixedString Field, typename ModelT>
    [[nodiscard]] static ModelFieldState fieldState(const ModelT& model) {
        return model.template ruviaFieldState<Field>();
    }

    template <typename ModelT, typename ValidatorT>
    static void validateRequired(const ModelT& model, std::string_view prefix, ValidatorT& validator) {
        model.ruviaValidateRequired(prefix, validator);
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail::model {

template <typename... RuleTs>
class Rules final {
public:
    constexpr explicit Rules(RuleTs... rules) noexcept
        : rules_(rules...) {
        static_assert((isValidationRule<RuleTs>() && ...),
            "RUVIA_RULE accepts only validation rules such as RUVIA_REQUIRED, RUVIA_MIN, "
            "RUVIA_MAX, "
            "RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, RUVIA_REGEX, RUVIA_MATCH, RUVIA_CUSTOM, "
            "RUVIA_NESTED, and RUVIA_EACH.");
    }

    [[nodiscard]] constexpr bool required() const noexcept {
        return (isRequiredRule<RuleTs>() || ... || false);
    }

    [[nodiscard]] constexpr std::string_view requiredMessage() const noexcept {
        std::string_view result{"is required"};
        std::apply([&result](const auto&... rules) { (setRequiredMessage(result, rules), ...); }, rules_);
        return result;
    }

    template <typename OptionalT, typename ValidatorT>
    void validate(ModelFieldState state, const OptionalT& value, std::string_view path, ValidatorT& validator) const {
        if (state == ModelFieldState::kDuplicate) {
            validator.add(path, "duplicate", "is duplicated");
            return;
        }
        if (state == ModelFieldState::kInvalidType) {
            using FieldT = typename OptionalT::value_type;
            validator.add(path, "invalid_type", expectedTypeName<FieldT>());
            return;
        }
        if (!value) {
            if (required()) {
                validator.add(path, "required", requiredMessage());
            }
            return;
        }
        validatePresent(*value, path, validator);
    }

private:
    template <typename RuleT>
    static constexpr void setRequiredMessage(std::string_view& result, const RuleT& rule) noexcept {
        if constexpr (isRequiredRule<RuleT>()) {
            result = rule.message;
        } else {
            (void)result;
            (void)rule;
        }
    }

    template <typename ValueT, typename ValidatorT>
    void validatePresent(const ValueT& value, std::string_view path, ValidatorT& validator) const {
        std::apply([&value, path, &validator](const auto&... rules) { (validateRule(value, path, validator, rules), ...); }, rules_);
    }

    std::tuple<RuleTs...> rules_;
};

template <typename... RuleTs>
Rules(RuleTs...) -> Rules<RuleTs...>;

}  // namespace ruvia::detail::model
