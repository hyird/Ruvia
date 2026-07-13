#pragma once

#include <string_view>

#include "ruvia/web/detail/model/PatternCompiler.h"

namespace ruvia::detail::model {

struct Required final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is required"};
};

struct Min final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    std::string_view message{"is too small"};
};

struct Max final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    std::string_view message{"is too big"};
};

template <FixedString... Values>
struct OneOf final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is not allowed"};
};

struct Email final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"must be a valid email"};
};

template <typename PredicateT>
struct Custom final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"is invalid"};
    PredicateT predicate;
};

template <typename PredicateT>
struct Match final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"does not match"};
    PredicateT predicate;
};

template <typename ValidatorT>
struct Nested final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValidatorT>
struct Each final {
    using RuviaValidationRuleMarker = void;
};

template <typename ValueT>
struct Default final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    ValueT value;
};

template <typename ValueT>
Default(ValueT) -> Default<ValueT>;

struct OmitEmpty final {
    using RuviaModelOptionMarker = void;
};

struct EmitNull final {
    using RuviaModelOptionMarker = void;
};

template <FixedString Pattern>
struct PatternRule final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto plan = CompiledPatternPlan<Pattern>::value;

    std::string_view message{"has invalid format"};
};

template <FixedString Pattern>
struct RegexRule final {
    using RuviaValidationRuleMarker = void;

    std::string_view message{"has invalid format"};
};

}  // namespace ruvia::detail::model
