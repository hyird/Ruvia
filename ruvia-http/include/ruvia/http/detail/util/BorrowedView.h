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
[[nodiscard]] constexpr std::string_view httpBorrowedCStringView(const char* value) noexcept {
    return value == nullptr ? std::string_view{} : std::string_view(value);
}

template <typename T>
[[nodiscard]] constexpr std::string_view httpBorrowedView(const T& value) noexcept {
    using Value = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<Value, std::nullptr_t>) {
        return {};
    } else if constexpr (std::is_pointer_v<Value> &&
                         std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Value>>, char>) {
        return httpBorrowedCStringView(value);
    } else {
        return std::string_view(value);
    }
}

// Zero-copy protocol values frequently retain views into caller-owned character
// storage. Use this shared predicate on deleted overloads so a temporary
// std::string (including std::pmr::string) cannot silently satisfy a
// std::string_view parameter and leave the returned protocol value dangling.
template <typename T>
inline constexpr bool kIsHttpOwningCharString = false;

template <typename Traits, typename Allocator>
inline constexpr bool kIsHttpOwningCharString<std::basic_string<char, Traits, Allocator>> = true;

template <typename T>
concept HttpTemporaryOwningCharString =
    kIsHttpOwningCharString<std::remove_cvref_t<T>> && !std::is_lvalue_reference_v<T&&>;

}  // namespace ruvia::detail
