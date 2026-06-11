#pragma once

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <vector>

// Internal layer: included by ruvia/http/Model.h after model field types are
// declared. Users should include ruvia/http/Model.h instead of this file.

namespace ruvia::detail {

// Shared model traits used by parser, validation rules, and generated macros.

enum class ModelFieldState : std::uint8_t {
    kMissing,
    kParsed,
    kInvalidType,
    kDuplicate
};

template <typename>
inline constexpr bool alwaysFalse = false;

inline constexpr std::size_t kMaxJsonDepth = 64;

template <typename T>
inline constexpr bool isRuviaString = std::is_same_v<std::remove_cvref_t<T>, String>;

template <typename T>
struct RuviaArrayTraits {
    static constexpr bool value = false;
};

template <typename ValueT>
struct RuviaArrayTraits<std::pmr::vector<ValueT>> {
    static constexpr bool value = true;
    using value_type = ValueT;
};

template <typename T>
inline constexpr bool isRuviaArray = RuviaArrayTraits<std::remove_cvref_t<T>>::value;

template <typename T>
struct RuviaListTraits {
    static constexpr bool value = false;
};

template <typename ValueT>
struct RuviaListTraits<List<ValueT>> {
    static constexpr bool value = true;
    using value_type = ValueT;
};

template <typename T>
inline constexpr bool isRuviaList = RuviaListTraits<std::remove_cvref_t<T>>::value;

template <typename T>
struct RuviaScalarTraits {
    static constexpr bool value = false;
};

template <> struct RuviaScalarTraits<Bool> { static constexpr bool value = true; using value_type = bool; };
template <> struct RuviaScalarTraits<Float> { static constexpr bool value = true; using value_type = float; };
template <> struct RuviaScalarTraits<Double> { static constexpr bool value = true; using value_type = double; };
template <> struct RuviaScalarTraits<Int32> { static constexpr bool value = true; using value_type = std::int32_t; };
template <> struct RuviaScalarTraits<UInt32> { static constexpr bool value = true; using value_type = std::uint32_t; };
template <> struct RuviaScalarTraits<Int64> { static constexpr bool value = true; using value_type = std::int64_t; };
template <> struct RuviaScalarTraits<UInt64> { static constexpr bool value = true; using value_type = std::uint64_t; };

template <typename T>
inline constexpr bool isRuviaScalar = RuviaScalarTraits<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isFormField =
    isRuviaString<T> || isRuviaScalar<T>;

template <typename T>
inline constexpr bool isModelField =
    isRuviaString<T> || isRuviaArray<T> || isRuviaList<T> || JsonBody<std::remove_cvref_t<T>>::value ||
    isRuviaScalar<T>;

template <typename T>
[[nodiscard]] T makeRequestValue(std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T>) {
        return T(resource == nullptr ? std::pmr::get_default_resource() : resource);
    } else if constexpr (isRuviaArray<T>) {
        using ValueT = typename RuviaArrayTraits<std::remove_cvref_t<T>>::value_type;
        return T(std::pmr::polymorphic_allocator<ValueT>(
            resource == nullptr ? std::pmr::get_default_resource() : resource));
    } else if constexpr (isRuviaList<T>) {
        return T(resource == nullptr ? std::pmr::get_default_resource() : resource);
    } else if constexpr (JsonBody<std::remove_cvref_t<T>>::value) {
        return T(resource == nullptr ? std::pmr::get_default_resource() : resource);
    } else {
        (void)resource;
        return T{};
    }
}

}  // namespace ruvia::detail
