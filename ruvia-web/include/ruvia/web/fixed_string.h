#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

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
