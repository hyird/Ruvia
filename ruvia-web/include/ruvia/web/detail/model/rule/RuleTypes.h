#pragma once

#include <type_traits>

#include "ruvia/http/BorrowedText.h"
#include "ruvia/web/detail/model/pattern/PatternCompiler.h"

namespace ruvia::detail::model {

using ::ruvia::BorrowedText;
static_assert(std::is_same_v<BorrowedText, ::ruvia::BorrowedText>);

struct Required final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is required"};
};

struct Min final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    BorrowedText message{"is too small"};
};

struct Max final {
    using RuviaValidationRuleMarker = void;

    long double value{0};
    BorrowedText message{"is too big"};
};

template <FixedString... Values>
struct OneOf final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is not allowed"};
};

struct Email final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"must be a valid email"};
};

template <typename PredicateT>
struct Custom final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"is invalid"};
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

template <auto Provider>
struct StaticDefault final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    using value_type = std::remove_cvref_t<decltype(Provider())>;
    value_type value{Provider()};
};

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

    BorrowedText message{"has invalid format"};
};

template <FixedString Pattern>
struct RegexRule final {
    using RuviaValidationRuleMarker = void;

    BorrowedText message{"has invalid format"};
};

}  // namespace ruvia::detail::model
