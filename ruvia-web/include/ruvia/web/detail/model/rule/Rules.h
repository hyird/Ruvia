#pragma once

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/ModelSchema.h"
#include "ruvia/web/detail/model/rule/RulePack.h"

namespace ruvia::detail {

struct ModelValidationAccess final {
    template <FixedString Field, typename ModelT>
    [[nodiscard]] static ModelFieldState fieldState(const ModelT& model) {
        return model.template ruviaFieldState<Field>();
    }

    template <FixedString Field, typename ModelT>
    [[nodiscard]] static const auto& fieldValue(const ModelT& model) {
        return model.template ruviaFieldValue<Field>();
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
    static void validateStructure(
        const ModelT& modelValue, std::string_view prefix, ValidatorT& validator) {
        model::visitModelFields(
            modelValue, ModelT::ruviaSchema(), [&](const auto&, const auto& slot) {
                using SlotT = std::remove_cvref_t<decltype(slot)>;
                std::pmr::string path(validator.resource());
                model::appendPath(path, prefix, slot.wireName());

                switch (slot.state()) {
                    case ModelFieldState::kDuplicate:
                        validator.add(path, "duplicate", "is duplicated");
                        return;
                    case ModelFieldState::kInvalidType:
                        validator.add(path, "invalid_type",
                            model::expectedTypeName<typename SlotT::value_type>());
                        return;
                    case ModelFieldState::kMissing:
                        if constexpr (SlotT::required) {
                            validator.add(path, "required", "is required");
                        }
                        return;
                    case ModelFieldState::kNull:
                        return;
                    case ModelFieldState::kParsed:
                        break;
                }

                validateValueStructure(*slot.value(), path, validator);
            });
    }

    template <typename ModelT, typename ValidatorT>
    static void validateModel(const ModelT& modelValue, ValidatorT& validator) {
        validateStructure(modelValue, {}, validator);
        validateFieldRules(modelValue, {}, validator);
    }

    template <typename ModelT, typename ValidatorT>
    static void validateFieldRules(
        const ModelT& modelValue, std::string_view prefix, ValidatorT& validator) {
        model::visitModelFields(
            modelValue, ModelT::ruviaSchema(), [&](const auto& descriptor, const auto& slot) {
                using DescriptorT = std::remove_cvref_t<decltype(descriptor)>;
                std::pmr::string path(validator.resource());
                model::appendPath(path, prefix, slot.wireName());
                typename DescriptorT::rules_type{}.validate(
                    slot.state(), slot.value(), path, validator);

                if (slot.state() != ModelFieldState::kParsed || !slot.value()) {
                    return;
                }
                validateNestedFieldRules(*slot.value(), path, validator);
            });
    }

private:
    template <typename ValueT>
    [[nodiscard]] static bool valueStructureValid(const ValueT& value) {
        using T = std::remove_cvref_t<ValueT>;
        if constexpr (isRequestModel<T>) {
            return structureValid(value);
        } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
            using ElementT = typename T::value_type;
            if constexpr (isRequestModel<ElementT> || isRuviaArray<ElementT> || isRuviaBoxedArray<ElementT>) {
                for (const auto& element : value) {
                    if (!valueStructureValid(element)) {
                        return false;
                    }
                }
            }
            return true;
        } else {
            return true;
        }
    }

    template <typename ValueT, typename ValidatorT>
    static void validateValueStructure(
        const ValueT& value, std::string_view path, ValidatorT& validator) {
        using T = std::remove_cvref_t<ValueT>;
        if constexpr (isRequestModel<T>) {
            validateStructure(value, path, validator);
        } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
            using ElementT = typename T::value_type;
            if constexpr (isRequestModel<ElementT> || isRuviaArray<ElementT> || isRuviaBoxedArray<ElementT>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    std::pmr::string itemPath(validator.resource());
                    model::appendIndexPath(itemPath, path, index++);
                    validateValueStructure(element, itemPath, validator);
                }
            }
        }
    }

    template <typename ValueT, typename ValidatorT>
    static void validateNestedFieldRules(
        const ValueT& value, std::string_view path, ValidatorT& validator) {
        using T = std::remove_cvref_t<ValueT>;
        if constexpr (isRequestModel<T>) {
            validateFieldRules(value, path, validator);
        } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
            using ElementT = typename T::value_type;
            if constexpr (isRequestModel<ElementT> || isRuviaArray<ElementT> || isRuviaBoxedArray<ElementT>) {
                std::size_t index = 0;
                for (const auto& element : value) {
                    std::pmr::string itemPath(validator.resource());
                    model::appendIndexPath(itemPath, path, index++);
                    validateNestedFieldRules(element, itemPath, validator);
                }
            }
        }
    }
};

}  // namespace ruvia::detail
