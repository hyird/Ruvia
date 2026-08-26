#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/web/detail/model/pattern/PatternMatcher.h"
#include "ruvia/web/detail/model/pattern/RegexMatcher.h"
#include "ruvia/web/detail/model/rule/RuleSupport.h"

namespace ruvia::detail::model {

template <typename ValueT, typename ValidatorT>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Required&) {
    (void)value;
    (void)path;
    (void)validator;
}

template <typename ValueT, typename ValidatorT>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Min& rule) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        if (modelSize(value) < static_cast<std::size_t>(rule.value)) {
            validator.add(path, "too_small", rule.message);
        }
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        if (modelNumber(value) < rule.value) {
            validator.add(path, "too_small", rule.message);
        }
    }
}

template <typename ValueT, typename ValidatorT>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Max& rule) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        if (modelSize(value) > static_cast<std::size_t>(rule.value)) {
            validator.add(path, "too_big", rule.message);
        }
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        if (modelNumber(value) > rule.value) {
            validator.add(path, "too_big", rule.message);
        }
    }
}

template <typename ValueT, typename ValidatorT, FixedString... Values>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const OneOf<Values...>& rule) {
    const auto actual = modelString(value);
    if (!((actual == Values.view()) || ...)) {
        validator.add(path, "one_of", rule.message);
    }
}

template <typename ValueT, typename ValidatorT>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Email& rule) {
    if (!isEmailLike(modelString(value))) {
        validator.add(path, "email", rule.message);
    }
}

template <typename ValueT, typename ValidatorT, FixedString Pattern>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const PatternRule<Pattern>& rule) {
    const auto actual = modelString(value);
    if (!matchPatternPlan<Pattern>(actual)) {
        validator.add(path, "pattern", rule.message);
    }
}

template <typename ValueT, typename ValidatorT, FixedString Pattern>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const RegexRule<Pattern>& rule) {
    const auto actual = modelString(value);
    if (!matchRegexPattern<Pattern>(actual)) {
        validator.add(path, "regex", rule.message);
    }
}

template <typename ValueT, typename ValidatorT, typename PredicateT>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const Custom<PredicateT>& rule) {
    if (!rule.predicate(value)) {
        validator.add(path, "custom", rule.message);
    }
}

template <typename ValueT, typename ValidatorT, typename ValidationSchemaT>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const Nested<ValidationSchemaT>&) {
    static_assert(
        requires(const ValidationSchemaT& schema, const ValueT& nested, std::string_view nestedPath,
            ValidatorT& nestedValidator) {
            schema.validateNested(nested, nestedPath, nestedValidator);
        },
        "RUVIA_NESTED validator must provide validateNested(const FieldT&, std::string_view, "
        "ruvia::Validator&)");
    ValidationSchemaT schema;
    schema.validateNested(value, path, validator);
}

template <typename ValueT, typename ValidatorT, typename ValidationSchemaT>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const Each<ValidationSchemaT>&) {
    static_assert(detail::isRuviaArray<ValueT> || detail::isRuviaBoxedArray<ValueT>,
        "RUVIA_EACH can only validate ruvia::Array<T> or ruvia::BoxedArray<T> fields");
    static_assert(
        requires(const ValidationSchemaT& schema,
            const typename std::remove_cvref_t<ValueT>::value_type& item, std::string_view itemPath,
            ValidatorT& nestedValidator) {
            schema.validateNested(item, itemPath, nestedValidator);
        },
        "RUVIA_EACH validator must provide validateNested(const ItemT&, std::string_view, "
        "ruvia::Validator&)");

    ValidationSchemaT schema;
    std::size_t index = 0;
    for (const auto& item : value) {
        std::pmr::string itemPath(validator.resource());
        appendIndexPath(itemPath, path, index);
        schema.validateNested(item, itemPath, validator);
        ++index;
    }
}

template <typename ValueT, typename ValidatorT, typename RuleT>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator, const RuleT&)
    requires(isDefaultRule<RuleT>() || std::is_same_v<std::remove_cvref_t<RuleT>, OmitEmpty> ||
             std::is_same_v<std::remove_cvref_t<RuleT>, EmitNull>)
{
    (void)value;
    (void)path;
    (void)validator;
}

}  // namespace ruvia::detail::model
