#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/pattern/PatternMatcher.h"
#include "ruvia/web/detail/model/pattern/RegexMatcher.h"
#include "ruvia/web/detail/model/rule/RuleSupport.h"

namespace ruvia::detail::model {

template <auto Bound>
[[nodiscard]] consteval bool validModelSizeBound() {
    if constexpr (std::is_integral_v<decltype(Bound)>) {
        return std::in_range<std::size_t>(Bound);
    } else {
        return false;
    }
}

template <auto Bound, typename ValueT>
[[nodiscard]] bool belowModelBound(const ValueT& value) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        static_assert(validModelSizeBound<Bound>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return modelSize(value) < static_cast<std::size_t>(Bound);
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        using NumberT = detail::ModelScalarValueT<ValueT>;
        if constexpr (std::is_integral_v<NumberT>) {
            static_assert(std::is_integral_v<decltype(Bound)>,
                "integer fields require an integral bound");
            return std::cmp_less(value.value, Bound);
        } else {
            return static_cast<long double>(value.value) < static_cast<long double>(Bound);
        }
    } else {
        static_assert(detail::alwaysFalse<ValueT>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <auto Bound, typename ValueT>
[[nodiscard]] bool aboveModelBound(const ValueT& value) {
    if constexpr (modelHasSizeRule<ValueT>()) {
        static_assert(validModelSizeBound<Bound>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return modelSize(value) > static_cast<std::size_t>(Bound);
    } else if constexpr (modelHasNumberRule<ValueT>()) {
        using NumberT = detail::ModelScalarValueT<ValueT>;
        if constexpr (std::is_integral_v<NumberT>) {
            static_assert(std::is_integral_v<decltype(Bound)>,
                "integer fields require an integral bound");
            return std::cmp_greater(value.value, Bound);
        } else {
            return static_cast<long double>(value.value) > static_cast<long double>(Bound);
        }
    } else {
        static_assert(detail::alwaysFalse<ValueT>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <typename ValueT, typename ValidatorT, auto Bound, FixedString Message>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Min<Bound, Message>&) {
    if (belowModelBound<Bound>(value)) {
        validator.add(path, "too_small", Message.view());
    }
}

template <typename ValueT, typename ValidatorT, auto Bound, FixedString Message>
void validateRule(
    const ValueT& value, std::string_view path, ValidatorT& validator, const Max<Bound, Message>&) {
    if (aboveModelBound<Bound>(value)) {
        validator.add(path, "too_big", Message.view());
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
