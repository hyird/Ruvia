#pragma once

#include <concepts>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia {
namespace detail {

template <typename t_type>
inline constexpr bool is_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool is_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

template <typename t_type>
concept temporary_owning_char_string =
    is_owning_char_string<std::remove_cvref_t<t_type>> && !std::is_lvalue_reference_v<t_type&&>;

template <typename range_type>
concept temporary_owning_byte_range =
    !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> &&
    !std::ranges::borrowed_range<range_type> &&
    std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, std::byte>;

}  // namespace detail

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <detail::temporary_owning_char_string string_type>
std::span<const std::byte> as_bytes(string_type&&) = delete;

[[nodiscard]] inline std::string_view as_chars(std::span<const std::byte> bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <detail::temporary_owning_byte_range range_type>
std::string_view as_chars(range_type&&) = delete;

}  // namespace ruvia
