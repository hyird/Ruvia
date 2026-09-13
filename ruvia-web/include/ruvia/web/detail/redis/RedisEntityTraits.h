#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/detail/model/Traits.h"

namespace ruvia::detail {

// Redis stores the scalar representation of a database entity column in a
// hash.  Keep this vocabulary here instead of coupling Redis to a separate
// public entity declaration: the same DbColumn type is used by SQL and Redis.
template <typename T>
inline constexpr bool isRedisEntityString =
    std::is_same_v<std::remove_cvref_t<T>, String> ||
    std::is_same_v<std::remove_cvref_t<T>, std::pmr::string>;

template <typename T>
inline constexpr bool isRedisEntityModelScalar =
    isRuviaScalar<std::remove_cvref_t<T>>;

template <typename T>
inline constexpr bool isRedisEntityNativeInteger =
    std::is_integral_v<std::remove_cvref_t<T>> &&
    !std::is_same_v<std::remove_cvref_t<T>, bool> &&
    !std::is_same_v<std::remove_cvref_t<T>, char> &&
    !std::is_same_v<std::remove_cvref_t<T>, signed char> &&
    !std::is_same_v<std::remove_cvref_t<T>, unsigned char> &&
    !std::is_same_v<std::remove_cvref_t<T>, wchar_t> &&
    !std::is_same_v<std::remove_cvref_t<T>, char8_t> &&
    !std::is_same_v<std::remove_cvref_t<T>, char16_t> &&
    !std::is_same_v<std::remove_cvref_t<T>, char32_t>;

template <typename T>
inline constexpr bool isRedisEntityNativeFloat =
    std::is_same_v<std::remove_cvref_t<T>, float> ||
    std::is_same_v<std::remove_cvref_t<T>, double>;

template <typename T>
inline constexpr bool isRedisEntityNativeNumber =
    isRedisEntityNativeInteger<T> || isRedisEntityNativeFloat<T>;

template <typename T, bool = isRedisEntityModelScalar<T>>
struct RedisEntityModelInteger final : std::false_type {};

template <typename T>
struct RedisEntityModelInteger<T, true> final
    : std::bool_constant<
          std::is_integral_v<ModelScalarValueT<std::remove_cvref_t<T>>> &&
          !std::is_same_v<ModelScalarValueT<std::remove_cvref_t<T>>, bool>> {};

template <typename T>
inline constexpr bool isRedisEntityModelInteger = RedisEntityModelInteger<T>::value;

template <typename T>
inline constexpr bool isRedisEntityPrimaryKeyScalar =
    isRedisEntityString<T> || isRedisEntityNativeInteger<T> || isRedisEntityModelInteger<T>;

template <typename T>
inline constexpr bool isRedisEntityNumber =
    isRedisEntityNativeNumber<T> ||
    (isRedisEntityModelScalar<T> && !std::is_same_v<std::remove_cvref_t<T>, Bool>);

template <typename T>
inline constexpr bool isRedisEntityScalar =
    isRedisEntityString<T> || std::is_same_v<std::remove_cvref_t<T>, bool> ||
    isRedisEntityNativeNumber<T> || isRedisEntityModelScalar<T>;

struct RedisEntityFieldOptions final {
    bool id{false};
    bool nullable{false};
    constexpr bool operator==(const RedisEntityFieldOptions&) const = default;
};

template <typename Column>
struct RedisEntityFieldAdapter final {
    static constexpr auto name = Column::name;
    using value_type = typename Column::value_type;
    using column_type = Column;
    static constexpr RedisEntityFieldOptions options{
        .id = Column::options.primaryKey,
        .nullable = Column::options.nullable,
    };
};

template <typename Entity, std::size_t... I>
consteval bool redisEntityColumnsAreScalar(std::index_sequence<I...>) {
    return (isRedisEntityScalar<
                typename std::tuple_element_t<I, typename Entity::Columns>::value_type> &&
            ...);
}

template <typename Entity>
consteval bool redisEntityColumnsAreScalar() {
    return redisEntityColumnsAreScalar<Entity>(
        std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

template <typename Entity, std::size_t... I>
consteval std::size_t redisEntityPrimaryKeyCount(std::index_sequence<I...>) {
    return (std::size_t{
                std::tuple_element_t<I, typename Entity::Columns>::options.primaryKey} +
            ... +
            std::size_t{0});
}

template <typename Entity>
consteval std::size_t redisEntityPrimaryKeyCount() {
    return redisEntityPrimaryKeyCount<Entity>(
        std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

template <typename Entity, std::size_t... I>
consteval bool redisEntityPrimaryKeyIsValid(std::index_sequence<I...>) {
    return ((
                !std::tuple_element_t<I, typename Entity::Columns>::options.primaryKey ||
                (isRedisEntityPrimaryKeyScalar<typename std::tuple_element_t<
                        I, typename Entity::Columns>::value_type> &&
                    !std::tuple_element_t<I, typename Entity::Columns>::options.nullable)) &&
            ...);
}

template <typename Entity>
consteval bool redisEntityPrimaryKeyIsValid() {
    return redisEntityPrimaryKeyIsValid<Entity>(
        std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

template <typename Entity>
consteval void validateRedisEntity() {
    static_assert(redisEntityColumnsAreScalar<Entity>(),
        "Redis repositories currently support only scalar database columns");
    static_assert(redisEntityPrimaryKeyCount<Entity>() == 1,
        "Redis entities must declare exactly one primary-key database column");
    static_assert(redisEntityPrimaryKeyIsValid<Entity>(),
        "Redis primary keys must be non-nullable string or integer columns");
}

}  // namespace ruvia::detail
