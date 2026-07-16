#pragma once

#include <string>
#include <type_traits>

namespace ruvia::detail {

template <typename T>
struct IsCharBasicString final : std::false_type {};

template <typename Traits, typename Allocator>
struct IsCharBasicString<std::basic_string<char, Traits, Allocator>> final
    : std::true_type {};

template <typename T>
concept RvalueCharBasicString =
    !std::is_lvalue_reference_v<T&&> &&
    IsCharBasicString<std::remove_cvref_t<T>>::value;

}  // namespace ruvia::detail
