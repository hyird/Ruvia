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
struct RuviaBoxedArrayTraits : std::false_type {};

template <typename ValueT>
struct RuviaBoxedArrayTraits<BoxedArray<ValueT>> : std::true_type {
    using value_type = ValueT;
};

template <typename T>
inline constexpr bool isRuviaBoxedArray = RuviaBoxedArrayTraits<std::remove_cvref_t<T>>::value;

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
using ModelScalarValueT = typename RuviaScalarTraits<std::remove_cvref_t<T>>::value_type;

template <typename T>
inline constexpr bool isFormField = isRuviaString<T> || isRuviaScalar<T>;

template <typename T, typename = void>
struct ResponseModel : std::false_type {};

template <typename T>
    requires requires { typename T::RuviaResponseModelSchema; }
struct ResponseModel<T, void> : std::true_type {};

template <typename T>
struct RuviaRequestModelFieldTraits : std::bool_constant<isRuviaString<T> || isRuviaScalar<T> || JsonBody<std::remove_cvref_t<T>>::value> {};

template <typename ValueT>
struct RuviaRequestModelFieldTraits<std::pmr::vector<ValueT>> : RuviaRequestModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename ValueT>
struct RuviaRequestModelFieldTraits<BoxedArray<ValueT>> : RuviaRequestModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename T>
inline constexpr bool isRequestModelField = RuviaRequestModelFieldTraits<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isRequestModel = JsonBody<std::remove_cvref_t<T>>::value;

template <typename T>
inline constexpr bool isResponseModel = ResponseModel<std::remove_cvref_t<T>>::value;

template <typename T>
struct RuviaResponseModelFieldTraits : std::bool_constant<isRuviaString<T> || isRuviaScalar<T> || isResponseModel<T>> {};

template <typename ValueT>
struct RuviaResponseModelFieldTraits<std::pmr::vector<ValueT>> : RuviaResponseModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename ValueT>
struct RuviaResponseModelFieldTraits<BoxedArray<ValueT>> : RuviaResponseModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename T>
inline constexpr bool isResponseModelField = RuviaResponseModelFieldTraits<std::remove_cvref_t<T>>::value;

template <typename T>
[[nodiscard]] T makeRequestValue(ResolvedPmrResourceTag, std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T>) {
        return ModelValueFactory::makeString(resource);
    } else if constexpr (isRuviaArray<T>) {
        using ValueT = typename RuviaArrayTraits<std::remove_cvref_t<T>>::value_type;
        return T(std::pmr::polymorphic_allocator<ValueT>(resource));
    } else if constexpr (isRuviaBoxedArray<T>) {
        return ModelValueFactory::makeBoxedArray<T>(resource);
    } else if constexpr (isRequestModel<T> || isResponseModel<T>) {
        return T(ModelOptions{.resource = resource});
    } else {
        (void)resource;
        return T{};
    }
}

template <typename T>
[[nodiscard]] T makeRequestValue(std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T> || isRuviaArray<T> || isRuviaBoxedArray<T> || isRequestModel<T> || isResponseModel<T>) {
        return makeRequestValue<T>(ResolvedPmrResourceTag{}, pmrResourceOrDefault(resource));
    } else {
        (void)resource;
        return T{};
    }
}

}  // namespace ruvia::detail
