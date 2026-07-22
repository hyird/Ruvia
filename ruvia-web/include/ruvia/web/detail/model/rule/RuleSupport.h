#pragma once

#include "ruvia/web/detail/model/rule/RuleTypes.h"

#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia::detail::model {

template <typename T>
[[nodiscard]] std::size_t modelSize(const T& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view().size();
    } else {
        return value.size();
    }
}

template <typename T>
[[nodiscard]] long double modelNumber(const T& value) noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaScalar<ValueT>) {
        return static_cast<long double>(value.value);
    } else {
        return static_cast<long double>(value);
    }
}

template <typename T>
[[nodiscard]] std::string_view modelString(const T& value) noexcept {
    if constexpr (requires { value.view(); }) {
        return value.view();
    } else {
        return std::string_view(value);
    }
}

[[nodiscard]] inline bool isEmailLike(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= value.size()) {
        return false;
    }
    const auto dot = value.find('.', at + 1);
    if (dot == std::string_view::npos || dot + 1 >= value.size()) {
        return false;
    }
    for (const char c : value) {
        // Compare as unsigned: `char` is signed on most targets, so a UTF-8 byte
        // (>= 0x80) is negative and would satisfy `c <= 0x20`, wrongly rejecting an
        // internationalized address (RFC 6531) as if it held a control byte. The
        // guard only means to reject controls, SP, and DEL -- match the codebase's
        // other byte checks (e.g. isValidCookieValue) by using an unsigned byte.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

template <typename T>
[[nodiscard]] bool isEmptyValue(const T& value) noexcept {
    if constexpr (detail::isRuviaString<T> || detail::isRuviaArray<T> || detail::isRuviaList<T>) {
        return value.empty();
    } else {
        return false;
    }
}

template <typename T>
[[nodiscard]] constexpr std::string_view expectedTypeName() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaString<ValueT>) {
        return "must be a string";
    } else if constexpr (detail::isRuviaArray<ValueT> || detail::isRuviaList<ValueT>) {
        return "must be an array";
    } else if constexpr (JsonBody<ValueT>::value) {
        return "must be an object";
    } else if constexpr (detail::isRuviaScalar<ValueT>) {
        using ScalarT = typename detail::RuviaScalarTraits<ValueT>::value_type;
        if constexpr (std::is_same_v<ScalarT, bool>) {
            return "must be a boolean";
        } else {
            return "must be a number";
        }
    } else {
        return "has invalid type";
    }
}

template <typename T>
[[nodiscard]] constexpr bool modelHasSizeRule() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    return detail::isRuviaString<ValueT> || detail::isRuviaArray<ValueT> || detail::isRuviaList<ValueT>;
}

template <typename T>
[[nodiscard]] constexpr bool modelHasNumberRule() noexcept {
    using ValueT = std::remove_cvref_t<T>;
    if constexpr (detail::isRuviaScalar<ValueT>) {
        using ScalarT = typename detail::RuviaScalarTraits<ValueT>::value_type;
        return std::is_arithmetic_v<ScalarT> && !std::is_same_v<ScalarT, bool>;
    } else {
        return std::is_arithmetic_v<ValueT> && !std::is_same_v<ValueT, bool>;
    }
}

inline void appendPath(
    std::pmr::string& output,
    std::string_view prefix,
    std::string_view field) {
    output.clear();
    output.reserve(prefix.size() + (prefix.empty() ? 0 : 1) + field.size());
    if (!prefix.empty()) {
        output.append(prefix.data(), prefix.size());
        output.push_back('.');
    }
    output.append(field.data(), field.size());
}

inline void appendIndexPath(
    std::pmr::string& output,
    std::string_view prefix,
    std::size_t index) {
    output.clear();
    output.reserve(prefix.size() + 2 + 20);
    output.append(prefix.data(), prefix.size());
    output.push_back('[');
    char buffer[32];
    const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), index);
    if (ec == std::errc{}) {
        output.append(buffer, static_cast<std::size_t>(ptr - buffer));
    }
    output.push_back(']');
}

template <typename Rule>
[[nodiscard]] constexpr bool isRequiredRule() noexcept {
    return std::is_same_v<std::remove_cvref_t<Rule>, Required>;
}

template <typename Rule>
[[nodiscard]] constexpr bool isDefaultRule() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaDefaultRuleMarker; };
}

template <typename Rule>
[[nodiscard]] constexpr bool isModelOption() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaModelOptionMarker; };
}

template <typename Rule>
[[nodiscard]] constexpr bool isValidationRule() noexcept {
    using RuleT = std::remove_cvref_t<Rule>;
    return requires { typename RuleT::RuviaValidationRuleMarker; };
}

}  // namespace ruvia::detail::model
