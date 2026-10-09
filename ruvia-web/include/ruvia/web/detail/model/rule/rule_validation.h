#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/pattern/pattern_matcher.h"
#include "ruvia/web/detail/model/pattern/regex_matcher.h"
#include "ruvia/web/detail/model/rule/rule_support.h"

namespace ruvia::detail::model {

template <auto bound_type>
[[nodiscard]] consteval bool valid_model_size_bound() {
    if constexpr (std::is_integral_v<decltype(bound_type)>) {
        return std::in_range<std::size_t>(bound_type);
    } else {
        return false;
    }
}

template <auto bound_type, typename value_t_type>
[[nodiscard]] bool below_model_bound(const value_t_type& value) {
    if constexpr (model_has_size_rule<value_t_type>()) {
        static_assert(valid_model_size_bound<bound_type>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return model_size(value) < static_cast<std::size_t>(bound_type);
    } else if constexpr (model_has_number_rule<value_t_type>()) {
        using number_t_type = detail::model_scalar_value_t_type<value_t_type>;
        if constexpr (std::is_integral_v<number_t_type>) {
            static_assert(std::is_integral_v<decltype(bound_type)>,
                "integer fields require an integral bound");
            return std::cmp_less(value.value_, bound_type);
        } else {
            return static_cast<long double>(value.value_) < static_cast<long double>(bound_type);
        }
    } else {
        static_assert(detail::always_false<value_t_type>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <auto bound_type, typename value_t_type>
[[nodiscard]] bool above_model_bound(const value_t_type& value) {
    if constexpr (model_has_size_rule<value_t_type>()) {
        static_assert(valid_model_size_bound<bound_type>(),
            "size rules require a nonnegative integral bound representable as size_t");
        return model_size(value) > static_cast<std::size_t>(bound_type);
    } else if constexpr (model_has_number_rule<value_t_type>()) {
        using number_t_type = detail::model_scalar_value_t_type<value_t_type>;
        if constexpr (std::is_integral_v<number_t_type>) {
            static_assert(std::is_integral_v<decltype(bound_type)>,
                "integer fields require an integral bound");
            return std::cmp_greater(value.value_, bound_type);
        } else {
            return static_cast<long double>(value.value_) > static_cast<long double>(bound_type);
        }
    } else {
        static_assert(detail::always_false<value_t_type>,
            "RUVIA_MIN/MAX require a numeric, string, bytes, or array field");
    }
}

template <typename value_t_type, typename validator_t_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_t_type& value, std::string_view path, validator_t_type& validator_value, const min<bound_type, message>&) {
    if (below_model_bound<bound_type>(value)) {
        validator_value.add(path, "too_small", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, auto bound_type, fixed_string message>
void validate_rule(
    const value_t_type& value, std::string_view path, validator_t_type& validator_value, const max<bound_type, message>&) {
    if (above_model_bound<bound_type>(value)) {
        validator_value.add(path, "too_big", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, fixed_string message, fixed_string... values>
void validate_rule(const value_t_type& value, std::string_view path, validator_t_type& validator_value,
    const one_of<message, values...>&) {
    const auto actual = model_string(value);
    if (!((actual == values.view()) || ...)) {
        validator_value.add(path, "one_of", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, fixed_string message>
void validate_rule(
    const value_t_type& value, std::string_view path, validator_t_type& validator_value, const email<message>&) {
    if (!is_email_like(model_string(value))) {
        validator_value.add(path, "email", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, fixed_string pattern, fixed_string message>
void validate_rule(const value_t_type& value, std::string_view path, validator_t_type& validator_value,
    const pattern_rule<pattern, message>&) {
    const auto actual = model_string(value);
    if (!match_pattern_plan<pattern>(actual)) {
        validator_value.add(path, "pattern", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, fixed_string pattern, fixed_string message>
void validate_rule(const value_t_type& value, std::string_view path, validator_t_type& validator_value,
    const regex_rule<pattern, message>&) {
    const auto actual = model_string(value);
    if (!match_regex_pattern<pattern>(actual)) {
        validator_value.add(path, "regex", message.view());
    }
}

template <typename value_t_type, typename validator_t_type, auto predicate, fixed_string message>
void validate_rule(const value_t_type& value, std::string_view path, validator_t_type& validator_value,
    const custom<predicate, message>&) {
    if (!predicate(value)) {
        validator_value.add(path, "custom", message.view());
    }
}

}  // namespace ruvia::detail::model
