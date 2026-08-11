#pragma once

#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/rule/RuleValidation.h"
#include "ruvia/web/detail/model/ModelSchema.h"

namespace ruvia::detail {

struct ModelValidationAccess final {
    template <FixedString Field, typename ModelT>
    [[nodiscard]] static ModelFieldState fieldState(const ModelT& model) {
        return model.template ruviaFieldState<Field>();
    }

    template <typename ModelT>
    [[nodiscard]] static bool structureValid(const ModelT& model) {
        bool valid = true;
        model::visitModelFields(model, ModelT::ruviaSchema(), [&](const auto&, const auto& slot) {
            using SlotT = std::remove_cvref_t<decltype(slot)>;
            const auto state = slot.state();
            if (state == ModelFieldState::kDuplicate || state == ModelFieldState::kInvalidType ||
                (SlotT::required && state == ModelFieldState::kMissing)) {
                valid = false;
                return;
            }
            if (const auto& value = slot.value(); value && !valueStructureValid(*value)) {
                valid = false;
            }
        });
        return valid;
    }

    template <typename ModelT, typename ValidatorT>
    static void validateStructure(const ModelT& modelValue, std::string_view prefix, ValidatorT& validator) {
        model::visitModelFields(modelValue, ModelT::ruviaSchema(), [&](const auto&, const auto& slot) {
            using SlotT = std::remove_cvref_t<decltype(slot)>;
            std::pmr::string path(validator.resource());
            model::appendPath(path, prefix, slot.wireName());

            switch (slot.state()) {
                case ModelFieldState::kDuplicate:
                    validator.add(path, "duplicate", "is duplicated");
                    return;
                case ModelFieldState::kInvalidType:
                    validator.add(path, "invalid_type", model::expectedTypeName<typename SlotT::value_type>());
                    return;
                case ModelFieldState::kMissing:
                    if constexpr (SlotT::required) {
                        validator.add(path, "required", "is required");
                    }
                    return;
                case ModelFieldState::kParsed:
                    break;
            }

            validateValueStructure(*slot.value(), path, validator);
        });
    }

private:
    template <typename ValueT>
    [[nodiscard]] static bool valueStructureValid(const ValueT& value) {
        using T = std::remove_cvref_t<ValueT>;
        if constexpr (JsonBody<T>::value) {
            return structureValid(value);
        } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
            using ElementT = typename T::value_type;
            if constexpr (JsonBody<std::remove_cvref_t<ElementT>>::value) {
                for (const auto& element : value) {
                    if (!structureValid(element)) return false;
                }
            }
            return true;
        } else {
            return true;
        }
    }

    template <typename ValueT, typename ValidatorT>
    static void validateValueStructure(const ValueT& value, std::string_view path, ValidatorT& validator) {
        using T = std::remove_cvref_t<ValueT>;
        if constexpr (JsonBody<T>::value) {
            validateStructure(value, path, validator);
        } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
            using ElementT = typename T::value_type;
            if constexpr (JsonBody<std::remove_cvref_t<ElementT>>::value) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    std::pmr::string itemPath(validator.resource());
                    model::appendIndexPath(itemPath, path, index++);
                    validateStructure(element, itemPath, validator);
                }
            }
        }
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
            "RUVIA_ONE_OF, RUVIA_EMAIL, RUVIA_PATTERN, RUVIA_REGEX, RUVIA_CUSTOM, "
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
        (void)state;
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
