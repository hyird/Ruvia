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

template <typename ValueT, typename ValidatorT, long double Bound, FixedString Message>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Min<Bound, Message>&) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        if (modelSize(value) < static_cast<std::size_t>(Bound)) {
            validator.add(path, "too_small", Message.view());
        }
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        if (modelNumber(value) < Bound) {
            validator.add(path, "too_small", Message.view());
        }
    }
}

template <typename ValueT, typename ValidatorT, long double Bound, FixedString Message>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Max<Bound, Message>&) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        if (modelSize(value) > static_cast<std::size_t>(Bound)) {
            validator.add(path, "too_big", Message.view());
        }
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        if (modelNumber(value) > Bound) {
            validator.add(path, "too_big", Message.view());
        }
    }
}

template <typename ValueT, typename ValidatorT, FixedString Message, FixedString... Values>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const OneOf<Message, Values...>&) {
    const auto actual = modelString(value);
    if (!((actual == Values.view()) || ...)) {
        validator.add(path, "one_of", Message.view());
    }
}

template <typename ValueT, typename ValidatorT, FixedString Message>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Email<Message>&) {
    if (!isEmailLike(modelString(value))) {
        validator.add(path, "email", Message.view());
    }
}

template <typename ValueT, typename ValidatorT, FixedString Pattern, FixedString Message>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const PatternRule<Pattern, Message>&) {
    const auto actual = modelString(value);
    if (!matchPatternPlan<Pattern>(actual)) {
        validator.add(path, "pattern", Message.view());
    }
}

template <typename ValueT, typename ValidatorT, FixedString Pattern, FixedString Message>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const RegexRule<Pattern, Message>&) {
    const auto actual = modelString(value);
    if (!matchRegexPattern<Pattern>(actual)) {
        validator.add(path, "regex", Message.view());
    }
}

template <typename ValueT, typename ValidatorT, auto Predicate, FixedString Message>
void validateRule(const ValueT& value, std::string_view path, ValidatorT& validator,
    const Custom<Predicate, Message>&) {
    if (!Predicate(value)) {
        validator.add(path, "custom", Message.view());
    }
}

}  // namespace ruvia::detail::model
