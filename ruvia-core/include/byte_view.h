#pragma once

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ruvia {

namespace detail {

template <typename range_type, typename element_type>
concept temporary_owning_contiguous_range = !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && !std::ranges::borrowed_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>;

}  // namespace detail

#ifndef RUVIA_LIFETIMEBOUND
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[msvc::lifetimebound]]
#elif __has_cpp_attribute(gnu::lifetimebound)
#define RUVIA_LIFETIMEBOUND [[gnu::lifetimebound]]
#else
#define RUVIA_LIFETIMEBOUND
#endif
#else
#define RUVIA_LIFETIMEBOUND
#endif
#endif

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <typename range_type>
    requires detail::temporary_owning_contiguous_range<range_type, char>
std::span<const std::byte> as_bytes(range_type&&) = delete;

// Reject list-backed storage without intercepting an empty view written as {}.
template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, char>
std::span<const std::byte> as_bytes(std::initializer_list<element_type>) = delete;

[[nodiscard]] inline std::string_view as_chars(std::span<const std::byte> bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <typename range_type>
    requires detail::temporary_owning_contiguous_range<range_type, std::byte>
std::string_view as_chars(range_type&&) = delete;

// Reject list-backed storage without intercepting an empty span written as {}.
template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, std::byte>
std::string_view as_chars(std::initializer_list<element_type>) = delete;

}  // namespace ruvia
