#pragma once

#include <algorithm>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

enum class integer_parse_error {
    invalid_format,
    out_of_range,
};

namespace detail {
template <typename t_type>
concept parsed_integer =
    std::integral<t_type> && !std::same_as<std::remove_cv_t<t_type>, bool> &&
    !std::same_as<std::remove_cv_t<t_type>, char> &&
    !std::same_as<std::remove_cv_t<t_type>, wchar_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char8_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char16_t> &&
    !std::same_as<std::remove_cv_t<t_type>, char32_t> &&
    requires(const char* first, const char* last, t_type& value) {
        std::from_chars(first, last, value, 10);
    };
}  // namespace detail

// Strict decimal input: no whitespace, leading '+', or trailing characters.
// A leading '-' is accepted only for signed integers. Format errors take
// precedence over overflow when the input also contains trailing characters.
template <detail::parsed_integer t_type>
[[nodiscard]] constexpr std::variant<t_type, integer_parse_error> parse_integer(std::string_view text) noexcept {
    if (text.empty()) {
        return integer_parse_error::invalid_format;
    }
    t_type value{};
    const auto* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value, 10);
    if (error == std::errc::invalid_argument || ptr != end) {
        return integer_parse_error::invalid_format;
    }
    if (error == std::errc::result_out_of_range) {
        return integer_parse_error::out_of_range;
    }
    return value;
}

}  // namespace ruvia

namespace ruvia {

template <std::size_t n>
struct fixed_string {
    char value_[n]{};

    constexpr fixed_string(const char (&text)[n]) noexcept {
        for (std::size_t i = 0; i < n; ++i) {
            value_[i] = text[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(value_, n - 1);
    }
    [[nodiscard]] constexpr std::string_view view() const&& = delete;
    [[nodiscard]] constexpr const char* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_;
    }
    [[nodiscard]] constexpr const char* data() const&& = delete;
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return n - 1;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return n == 1;
    }
    constexpr operator std::string_view() const& noexcept RUVIA_LIFETIMEBOUND {
        return view();
    }
    constexpr operator std::string_view() const&& = delete;
};

template <std::size_t n>
fixed_string(const char (&)[n]) -> fixed_string<n>;

template <std::size_t left_n, std::size_t right_n>
[[nodiscard]] constexpr bool operator==(
    const fixed_string<left_n>& left, const fixed_string<right_n>& right) noexcept {
    if constexpr (left_n != right_n) {
        return false;
    } else {
        return std::ranges::equal(left.value_, right.value_);
    }
}

namespace detail {

// Compile-time lookup shared by every schema keyed by fixed_string names
// (model fields, entity columns, database relations).

// Position of `name` in `names`, or names.size() when it is absent.
[[nodiscard]] consteval std::size_t fixed_name_index(
    std::span<const std::string_view> names, std::string_view name) noexcept {
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            return index;
        }
    }
    return names.size();
}

// Whether no two entries of `names` are equal under `equal`.
template <typename equal_type = std::equal_to<>>
[[nodiscard]] consteval bool fixed_names_distinct(
    std::span<const std::string_view> names, equal_type equal = {}) noexcept {
    for (std::size_t left = 0; left < names.size(); ++left) {
        for (std::size_t right = left + 1; right < names.size(); ++right) {
            if (equal(names[left], names[right])) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace detail

}  // namespace ruvia
