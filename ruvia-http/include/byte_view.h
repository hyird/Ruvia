#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

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

namespace ruvia {

using byte_view = std::span<const std::byte>;
using mutable_byte_view = std::span<std::byte>;

namespace detail {

template <typename range_type, typename element_type>
concept temporary_owning_contiguous_range = !std::is_lvalue_reference_v<range_type&&> && std::ranges::contiguous_range<range_type> && !std::ranges::borrowed_range<range_type> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<range_type>>, element_type>;

}  // namespace detail

// HTTP and other octet streams are not Unicode text. These conversions are
// explicit views of the same bytes; they do not decode or validate charset.
[[nodiscard]] inline byte_view as_bytes(std::string_view text) noexcept {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}
template <typename range_type>
    requires detail::temporary_owning_contiguous_range<range_type, char>
byte_view as_bytes(range_type&&) = delete;

template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, char>
byte_view as_bytes(std::initializer_list<element_type>) = delete;

[[nodiscard]] inline byte_view as_bytes(std::span<const std::uint8_t> octets) noexcept {
    return std::as_bytes(octets);
}
template <typename range_type>
    requires detail::temporary_owning_contiguous_range<range_type, std::uint8_t>
byte_view as_bytes(range_type&&) = delete;

template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, std::uint8_t> && (!std::same_as<std::uint8_t, unsigned char>)
byte_view as_bytes(std::initializer_list<element_type>) = delete;

template <typename byte_type>
    requires(std::same_as<std::remove_cv_t<byte_type>, unsigned char> && !std::same_as<std::uint8_t, unsigned char>)
[[nodiscard]] inline byte_view as_bytes(std::span<byte_type> octets) noexcept {
    return std::as_bytes(octets);
}
template <typename range_type>
    requires(!std::same_as<std::uint8_t, unsigned char>) && detail::temporary_owning_contiguous_range<range_type, unsigned char>
byte_view as_bytes(range_type&&) = delete;

template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, unsigned char>
byte_view as_bytes(std::initializer_list<element_type>) = delete;

[[nodiscard]] inline mutable_byte_view as_mutable_bytes(std::span<char> buffer) noexcept {
    return std::as_writable_bytes(buffer);
}
[[nodiscard]] inline mutable_byte_view as_mutable_bytes(std::span<std::uint8_t> buffer) noexcept {
    return std::as_writable_bytes(buffer);
}

[[nodiscard]] inline std::string_view as_chars(byte_view bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()};
}
template <typename range_type>
    requires detail::temporary_owning_contiguous_range<range_type, std::byte>
std::string_view as_chars(range_type&&) = delete;

template <typename element_type>
    requires std::same_as<std::remove_cv_t<element_type>, std::byte>
std::string_view as_chars(std::initializer_list<element_type>) = delete;

}  // namespace ruvia
