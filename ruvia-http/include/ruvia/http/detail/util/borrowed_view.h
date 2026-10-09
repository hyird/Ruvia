#pragma once

#include <cstddef>
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

template <typename t_type>
[[nodiscard]] constexpr std::string_view http_borrowed_view(const t_type& value) noexcept {
    using value_type = std::remove_cv_t<t_type>;
    if constexpr (std::is_same_v<value_type, std::nullptr_t>) {
        return {};
    } else if constexpr (std::is_pointer_v<value_type> &&
                         std::is_same_v<std::remove_cv_t<std::remove_pointer_t<value_type>>, char>) {
        return http_borrowed_c_string_view(value);
    } else {
        return std::string_view(value);
    }
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

}  // namespace ruvia::detail
