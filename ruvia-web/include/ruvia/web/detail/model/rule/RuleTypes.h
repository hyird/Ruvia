#pragma once

#include <type_traits>

#include "ruvia/web/detail/model/pattern/PatternCompiler.h"

namespace ruvia::detail::model {

template <auto Value, FixedString Message>
struct Min final {
    static_assert(std::is_arithmetic_v<decltype(Value)> && !std::is_same_v<decltype(Value), bool>,
        "RUVIA_MIN requires a numeric bound");
    using RuviaValidationRuleMarker = void;

    static constexpr auto value = Value;
    static constexpr auto message = Message;
};

template <auto Value, FixedString Message>
struct Max final {
    static_assert(std::is_arithmetic_v<decltype(Value)> && !std::is_same_v<decltype(Value), bool>,
        "RUVIA_MAX requires a numeric bound");
    using RuviaValidationRuleMarker = void;

    static constexpr auto value = Value;
    static constexpr auto message = Message;
};

template <FixedString Message, FixedString... Values>
struct OneOf final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto message = Message;
};

template <FixedString Message>
struct Email final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto message = Message;
};

template <auto Predicate, FixedString Message>
struct Custom final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto predicate = Predicate;
    static constexpr auto message = Message;
};

template <auto Provider>
struct Default final {
    using RuviaDefaultRuleMarker = void;
    using RuviaModelOptionMarker = void;

    // Metadata is stateless. Evaluate only when consuming a missing optional
    // field, never while constructing or moving a model.
    [[nodiscard]] static constexpr decltype(auto) value() {
        return Provider();
    }
};

struct Nullable final {
    using RuviaModelOptionMarker = void;
};

struct OmitEmpty final {
    using RuviaModelOptionMarker = void;
};

struct EmitNull final {
    using RuviaModelOptionMarker = void;
};

template <FixedString Pattern, FixedString Message>
struct PatternRule final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto plan = CompiledPatternPlan<Pattern>::value;
    static constexpr auto message = Message;
};

template <FixedString Pattern, FixedString Message>
struct RegexRule final {
    using RuviaValidationRuleMarker = void;

    static constexpr auto message = Message;
};

}  // namespace ruvia::detail::model
