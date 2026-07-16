#pragma once

#include <string>
#include <type_traits>

namespace ruvia::detail {

// Zero-copy protocol values frequently retain views into caller-owned character
// storage. Use this shared predicate on deleted overloads so a temporary
// std::string (including std::pmr::string) cannot silently satisfy a
// std::string_view parameter and leave the returned protocol value dangling.
template <typename T>
inline constexpr bool kIsHttpOwningCharString = false;

template <typename Traits, typename Allocator>
inline constexpr bool kIsHttpOwningCharString<
    std::basic_string<char, Traits, Allocator>> = true;

template <typename T>
concept HttpTemporaryOwningCharString =
    kIsHttpOwningCharString<std::remove_cvref_t<T>> &&
    !std::is_lvalue_reference_v<T&&>;

}  // namespace ruvia::detail
