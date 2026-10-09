#pragma once

#include <type_traits>

#include "ruvia/web/detail/model/pattern/pattern_compiler.h"

namespace ruvia::detail::model {

template <auto bound, fixed_string error_message>
struct min final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MIN requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto value = bound;
    static constexpr auto message = error_message;
};

template <auto bound, fixed_string error_message>
struct max final {
    static_assert(std::is_arithmetic_v<decltype(bound)> && !std::is_same_v<decltype(bound), bool>,
        "RUVIA_MAX requires a numeric bound");
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto value = bound;
    static constexpr auto message = error_message;
};

template <fixed_string error_message, fixed_string... values>
struct one_of final {
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto message = error_message;
};

template <fixed_string error_message>
struct email final {
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto message = error_message;
};

template <auto validation_predicate, fixed_string error_message>
struct custom final {
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto predicate = validation_predicate;
    static constexpr auto message = error_message;
};

template <auto provider>
struct default_value final {
    using ruvia_default_rule_marker_type = void;
    using ruvia_model_option_marker_type = void;

    // Metadata is stateless. Evaluate only when consuming a missing optional
    // field, never while constructing or moving a model.
    [[nodiscard]] static constexpr decltype(auto) value() {
        return provider();
    }
};

struct nullable final {
    using ruvia_model_option_marker_type = void;
};

struct omit_empty final {
    using ruvia_model_option_marker_type = void;
};

struct emit_null final {
    using ruvia_model_option_marker_type = void;
};

template <fixed_string pattern, fixed_string error_message>
struct pattern_rule final {
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto plan = compiled_pattern_plan<pattern>::value;
    static constexpr auto message = error_message;
};

template <fixed_string pattern, fixed_string error_message>
struct regex_rule final {
    using ruvia_validation_rule_marker_type = void;

    static constexpr auto message = error_message;
};

}  // namespace ruvia::detail::model
