#pragma once

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <vector>

#include "ruvia/web/ModelTypes.h"
#include "ruvia/core/memory/PmrResource.h"

// Internal layer. Users should include ruvia/web/Model.h instead of this file.

namespace ruvia::detail {

// Shared model traits used by parser, validation rules, and generated macros.

enum class ModelFieldState : std::uint8_t { kMissing, kParsed, kInvalidType, kDuplicate };

template <typename>
inline constexpr bool alwaysFalse = false;

template <typename T>
inline constexpr bool isRuviaString = std::is_same_v<std::remove_cvref_t<T>, String>;

template <typename T>
struct RuviaArrayTraits : std::false_type {};

template <typename ValueT>
struct RuviaArrayTraits<std::pmr::vector<ValueT>> : std::true_type {
    using value_type = ValueT;
};

template <typename T>
inline constexpr bool isRuviaArray = RuviaArrayTraits<std::remove_cvref_t<T>>::value;

template <typename T>
struct RuviaListTraits : std::false_type {};

template <typename ValueT>
struct RuviaListTraits<List<ValueT>> : std::true_type {
    using value_type = ValueT;
};

template <typename T>
inline constexpr bool isRuviaList = RuviaListTraits<std::remove_cvref_t<T>>::value;

template <typename T>
struct RuviaScalarTraits : std::false_type {};

template <>
struct RuviaScalarTraits<Bool> : std::true_type {
    using value_type = bool;
};
template <>
struct RuviaScalarTraits<Float> : std::true_type {
    using value_type = float;
};
template <>
struct RuviaScalarTraits<Double> : std::true_type {
    using value_type = double;
};
template <>
struct RuviaScalarTraits<Int32> : std::true_type {
    using value_type = std::int32_t;
};
template <>
struct RuviaScalarTraits<UInt32> : std::true_type {
    using value_type = std::uint32_t;
};
template <>
struct RuviaScalarTraits<Int64> : std::true_type {
    using value_type = std::int64_t;
};
template <>
struct RuviaScalarTraits<UInt64> : std::true_type {
    using value_type = std::uint64_t;
};

template <typename T>
inline constexpr bool isRuviaScalar = RuviaScalarTraits<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isFormField = isRuviaString<T> || isRuviaScalar<T>;

template <typename T>
inline constexpr bool isRequestModelField = isRuviaString<T> || isRuviaArray<T> || isRuviaList<T> || JsonBody<std::remove_cvref_t<T>>::value || isRuviaScalar<T>;

template <typename T>
inline constexpr bool isRequestModel = JsonBody<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isResponseModel = JsonBody<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isResponseModelField = isRuviaString<T> || isRuviaArray<T> || isRuviaList<T> || isResponseModel<T> || isRuviaScalar<T>;

template <typename T>
[[nodiscard]] T makeRequestValue(ResolvedPmrResourceTag, std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T>) {
        return ModelValueFactory::makeString(resource);
    } else if constexpr (isRuviaArray<T>) {
        using ValueT = typename RuviaArrayTraits<std::remove_cvref_t<T>>::value_type;
        return T(std::pmr::polymorphic_allocator<ValueT>(resource));
    } else if constexpr (isRuviaList<T>) {
        return ModelValueFactory::makeList<T>(resource);
    } else if constexpr (JsonBody<std::remove_cvref_t<T>>::value) {
        return T(resource);
    } else {
        (void)resource;
        return T{};
    }
}

template <typename T>
[[nodiscard]] T makeRequestValue(std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T> || isRuviaArray<T> || isRuviaList<T> || JsonBody<std::remove_cvref_t<T>>::value) {
        return makeRequestValue<T>(ResolvedPmrResourceTag{}, pmrResourceOrDefault(resource));
    } else {
        (void)resource;
        return T{};
    }
}

}  // namespace ruvia::detail
