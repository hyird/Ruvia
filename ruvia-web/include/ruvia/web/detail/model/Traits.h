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

// The wrapper types (ruvia::Int32 and friends) carry the value in a .value
// member; a field declared with the plain type holds it directly. Both are
// model scalars, and the parse, validation, and serialization layers work off
// the two traits below rather than naming either form.
template <typename T>
inline constexpr bool isWrappedModelScalar = RuviaScalarTraits<std::remove_cvref_t<T>>::value;

// The standard types a field may be declared with. Deliberately an explicit
// list rather than std::is_arithmetic: char and the sized character types carry
// text, not numbers, and admitting them would silently parse a string field as
// an integer.
template <typename T>
inline constexpr bool isPlainModelScalar =
    std::is_same_v<std::remove_cvref_t<T>, bool> || std::is_same_v<std::remove_cvref_t<T>, float> || std::is_same_v<std::remove_cvref_t<T>, double> ||
    std::is_same_v<std::remove_cvref_t<T>, std::int32_t> || std::is_same_v<std::remove_cvref_t<T>, std::uint32_t> ||
    std::is_same_v<std::remove_cvref_t<T>, std::int64_t> || std::is_same_v<std::remove_cvref_t<T>, std::uint64_t>;

template <typename T>
inline constexpr bool isRuviaScalar = isWrappedModelScalar<T> || isPlainModelScalar<T>;

// The arithmetic type a model scalar parses into and validates against: the
// wrapper's value_type, or the plain type itself.
template <typename T, bool Wrapped = isWrappedModelScalar<T>>
struct ModelScalarValue {
    using type = std::remove_cvref_t<T>;
};

template <typename T>
struct ModelScalarValue<T, true> {
    using type = typename RuviaScalarTraits<std::remove_cvref_t<T>>::value_type;
};

template <typename T>
using ModelScalarValueT = typename ModelScalarValue<T>::type;

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
