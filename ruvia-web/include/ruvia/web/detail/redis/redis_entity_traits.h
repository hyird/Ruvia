#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <type_traits>

#include "ruvia/web/model.h"

namespace ruvia::detail {

// Redis hash fields support owning scalar values and one string/integer key.
// Character types are text units, not numbers, so they are not Redis numbers.
template <typename t_type>
inline constexpr bool is_redis_entity_native_integer =
    is_model_integer<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_float =
    std::is_same_v<std::remove_cvref_t<t_type>, float> ||
    std::is_same_v<std::remove_cvref_t<t_type>, double>;

template <typename t_type>
inline constexpr bool is_redis_entity_native_number =
    is_redis_entity_native_integer<t_type> || is_redis_entity_native_float<t_type>;

// The one classification of Redis column value types: entity validation,
// index validation, query compilation and writes all derive from it.
enum class redis_column_kind : std::uint8_t {
    unsupported,
    text,
    bool_value,
    signed_value,
    unsigned_value,
    float32,
    float64,
};

template <typename t_type>
consteval redis_column_kind redis_number_kind() {
    if constexpr (std::is_same_v<t_type, float>) {
        return redis_column_kind::float32;
    } else if constexpr (std::is_same_v<t_type, double>) {
        return redis_column_kind::float64;
    } else if constexpr (std::is_signed_v<t_type>) {
        return redis_column_kind::signed_value;
    } else {
        return redis_column_kind::unsigned_value;
    }
}

template <typename t_type>
consteval redis_column_kind redis_column_value_kind() {
    using value_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<value_type, string> || std::is_same_v<value_type, std::pmr::string>) {
        return redis_column_kind::text;
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return redis_column_kind::bool_value;
    } else if constexpr (is_redis_entity_native_number<value_type>) {
        return redis_number_kind<value_type>();
    } else {
        return redis_column_kind::unsupported;
    }
}

template <typename t_type>
inline constexpr bool is_redis_entity_scalar = redis_column_value_kind<t_type>() != redis_column_kind::unsupported;

template <typename t_type>
inline constexpr bool is_redis_entity_primary_key_scalar =
    redis_column_value_kind<t_type>() == redis_column_kind::text ||
    redis_column_value_kind<t_type>() == redis_column_kind::signed_value ||
    redis_column_value_kind<t_type>() == redis_column_kind::unsigned_value;

}  // namespace ruvia::detail
