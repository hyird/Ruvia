#pragma once

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/ModelTypes.h"

// Internal layer. Users should include ruvia/web/Model.h instead of this file.

namespace ruvia {
class JsonObject;
class JsonValue;
}  // namespace ruvia

namespace ruvia::detail {

// Shared model traits used by parser, validation rules, and generated macros.

enum class ModelFieldState : std::uint8_t { kMissing,
    kParsed,
    kNull,
    kInvalidType,
    kDuplicate };

template <typename>
inline constexpr bool alwaysFalse = false;

template <typename T>
inline constexpr bool isRuviaString = std::is_same_v<std::remove_cvref_t<T>, String>;

template <typename T>
inline constexpr bool isRuviaBytes = std::is_same_v<std::remove_cvref_t<T>, Bytes>;

template <typename T>
inline constexpr bool isRuviaJsonValue = std::is_same_v<std::remove_cvref_t<T>, JsonValue>;

template <typename T>
inline constexpr bool isRuviaJsonObject = std::is_same_v<std::remove_cvref_t<T>, JsonObject>;

template <typename T>
struct RuviaArrayTraits : std::false_type {};

template <typename ValueT>
struct RuviaArrayTraits<Array<ValueT>> : std::true_type {
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
struct RuviaScalarTraits<Int8> : std::true_type {
    using value_type = std::int8_t;
};
template <>
struct RuviaScalarTraits<UInt8> : std::true_type {
    using value_type = std::uint8_t;
};
template <>
struct RuviaScalarTraits<Int16> : std::true_type {
    using value_type = std::int16_t;
};
template <>
struct RuviaScalarTraits<UInt16> : std::true_type {
    using value_type = std::uint16_t;
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

template <typename T>
inline constexpr bool isModel =
    requires { typename std::remove_cvref_t<T>::RuviaModelSchema; };

template <typename T>
struct RuviaModelFieldTraits
    : std::bool_constant<isRuviaString<T> || isRuviaBytes<T> || isRuviaScalar<T> ||
                         isRuviaJsonValue<T> || isRuviaJsonObject<T> || isModel<T>> {};

template <typename ValueT>
struct RuviaModelFieldTraits<Array<ValueT>>
    : RuviaModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename ValueT>
struct RuviaModelFieldTraits<BoxedArray<ValueT>>
    : RuviaModelFieldTraits<std::remove_cvref_t<ValueT>> {};

template <typename T>
inline constexpr bool isModelField = RuviaModelFieldTraits<std::remove_cvref_t<T>>::value;

template <typename T>
struct ModelJsonValueTraits
    : std::bool_constant<isRuviaString<T> || isRuviaBytes<T> || isRuviaScalar<T> || isModel<T>> {};

template <typename ValueT>
struct ModelJsonValueTraits<Array<ValueT>> : ModelJsonValueTraits<std::remove_cvref_t<ValueT>> {};

template <typename ValueT>
struct ModelJsonValueTraits<BoxedArray<ValueT>>
    : ModelJsonValueTraits<std::remove_cvref_t<ValueT>> {};

template <typename T>
inline constexpr bool isModelJsonValue =
    ModelJsonValueTraits<std::remove_cvref_t<T>>::value;

template <typename T>
[[nodiscard]] T makeRequestValue(ResolvedPmrResourceTag, std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T>) {
        return ModelValueFactory::makeString(resource);
    } else if constexpr (isRuviaBytes<T>) {
        return T(ModelOptions{.resource = resource});
    } else if constexpr (isRuviaArray<T>) {
        return T(ModelOptions{.resource = resource});
    } else if constexpr (isRuviaBoxedArray<T>) {
        return ModelValueFactory::makeBoxedArray<T>(resource);
    } else if constexpr (isModel<T> || isRuviaJsonValue<T> || isRuviaJsonObject<T>) {
        return T(ModelOptions{.resource = resource});
    } else {
        (void)resource;
        return T{};
    }
}

template <typename T>
[[nodiscard]] T makeRequestValue(std::pmr::memory_resource* resource) {
    if constexpr (isRuviaString<T> || isRuviaBytes<T> || isRuviaArray<T> || isRuviaBoxedArray<T> ||
                  isModel<T> || isRuviaJsonValue<T> || isRuviaJsonObject<T>) {
        return makeRequestValue<T>(ResolvedPmrResourceTag{}, pmrResourceOrDefault(resource));
    } else {
        (void)resource;
        return T{};
    }
}

}  // namespace ruvia::detail
