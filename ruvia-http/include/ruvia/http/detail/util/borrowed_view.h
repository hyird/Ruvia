#pragma once

#include <concepts>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia::detail {

// A C-string entry point is kept for literal-friendly APIs, but nullptr must
// not reach std::string_view(const char*)'s traits::length call. Normalizing it
// to the same empty view as the default wrapper keeps these noexcept borrowed
// values defined; consuming validators still decide whether empty is valid.
[[nodiscard]] constexpr std::string_view http_borrowed_c_string_view(const char* value) noexcept {
    return value == nullptr ? std::string_view{} : std::string_view(value);
}

// Zero-copy protocol values frequently retain views into caller-owned character
// storage. Use this shared predicate on deleted overloads so a temporary
// std::string (including std::pmr::string) cannot silently satisfy a
// std::string_view parameter and leave the returned protocol value dangling.
template <typename t_type>
inline constexpr bool is_http_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_http_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept http_temporary_owning_char_string =
    is_http_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

// std::span<const element_type> parameters bind every contiguous range,
// including temporary std::string, std::vector, and std::array values. Use this
// shared predicate on deleted overloads of span entry points whose results
// borrow the input; borrowed ranges (span, string_view) and lvalues still bind.
template <typename range_type, typename element_type>
concept http_temporary_owning_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>;

}  // namespace ruvia::detail
