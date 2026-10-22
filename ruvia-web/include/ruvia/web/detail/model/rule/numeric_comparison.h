#pragma once

#include <cmath>
#include <compare>
#include <limits>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail {

template <typename t_type>
inline constexpr bool is_validation_number =
    std::is_arithmetic_v<std::remove_cvref_t<t_type>> || is_ruvia_scalar<t_type>;

template <typename integer_type, typename floating_type>
[[nodiscard]] std::partial_ordering compare_validation_integer_floating(
    integer_type integer, floating_type floating) noexcept {
    if (std::isnan(floating)) {
        return std::partial_ordering::unordered;
    }

    // The upper bound is exclusive. Unlike max(), this power of two is
    // exactly representable even when the float cannot represent every integer.
    const auto upper = std::ldexp(floating_type{1}, std::numeric_limits<integer_type>::digits);
    if (floating >= upper) {
        return std::partial_ordering::less;
    }
    const auto lower = std::is_signed_v<integer_type> ? -upper : floating_type{0};
    if (floating < lower) {
        return std::partial_ordering::greater;
    }

    // NaN, infinity and out-of-range values have all returned before the cast.
    const auto truncated = static_cast<integer_type>(floating);
    if (std::cmp_less(integer, truncated)) {
        return std::partial_ordering::less;
    }
    if (std::cmp_greater(integer, truncated)) {
        return std::partial_ordering::greater;
    }
    const auto whole = std::trunc(floating);
    if (whole < floating) {
        return std::partial_ordering::less;
    }
    if (whole > floating) {
        return std::partial_ordering::greater;
    }
    return std::partial_ordering::equivalent;
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_less(const left_type& left, const right_type& right) {
    if constexpr (!is_validation_number<left_type> || !is_validation_number<right_type>) {
        return left < right;
    } else if constexpr (is_ruvia_scalar<left_type>) {
        return validation_less(left.value_, right);
    } else if constexpr (is_ruvia_scalar<right_type>) {
        return validation_less(left, right.value_);
    } else if constexpr (std::is_integral_v<left_type> && std::is_integral_v<right_type>) {
        return std::cmp_less(+left, +right);
    } else if constexpr (std::is_integral_v<left_type> && std::is_floating_point_v<right_type>) {
        return compare_validation_integer_floating(+left, right) == std::partial_ordering::less;
    } else if constexpr (std::is_floating_point_v<left_type> && std::is_integral_v<right_type>) {
        return compare_validation_integer_floating(+right, left) == std::partial_ordering::greater;
    } else {
        return left < right;
    }
}

template <typename left_type, typename right_type>
[[nodiscard]] bool validation_greater(const left_type& left, const right_type& right) {
    if constexpr (is_validation_number<left_type> && is_validation_number<right_type>) {
        return validation_less(right, left);
    } else {
        return left > right;
    }
}

}  // namespace ruvia::detail
