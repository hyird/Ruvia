#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "ruvia/web/detail/entity/value_storage.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail {

// Redis hash fields support owning scalar values and one string/integer key.
// Character types are text units, not numbers, so they are not Redis numbers.
template <typename t_type>
inline constexpr bool is_redis_entity_native_integer =
    std::is_integral_v<std::remove_cvref_t<t_type>> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, bool> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, signed char> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, unsigned char> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, wchar_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char8_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char16_t> &&
    !std::is_same_v<std::remove_cvref_t<t_type>, char32_t>;

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

// Model integer wrappers (int8, uint8) hold character-sized integers, so
// they are classified by their arithmetic type without the native exclusions.
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
    } else if constexpr (std::is_same_v<value_type, bool_value> || std::is_same_v<value_type, bool>) {
        return redis_column_kind::bool_value;
    } else if constexpr (is_ruvia_scalar<value_type>) {
        return redis_number_kind<model_scalar_value_t_type<value_type>>();
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

template <typename entity_type>
consteval bool redis_entity_columns_are_scalar() {
    bool result_value = true;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        result_value = result_value && is_redis_entity_scalar<typename column_type::value_type>;
    });
    return result_value;
}

template <typename entity_type>
consteval std::size_t redis_entity_primary_key_count() {
    std::size_t result_value = 0;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        result_value += column_type::options.primary_key_ ? 1U : 0U;
    });
    return result_value;
}

template <typename entity_type>
consteval bool redis_entity_primary_key_is_valid() {
    bool result_value = true;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            result_value = result_value && is_redis_entity_primary_key_scalar<typename column_type::value_type> &&
                           !column_type::options.nullable_;
        }
    });
    return result_value;
}

template <typename entity>
concept redis_entity_schema = requires(const entity& value) {
    typename entity::redis_entity_type;
    typename entity::columns_type;
    requires std::same_as<entity, typename entity::redis_entity_type>;
    { entity::prefix() } -> std::convertible_to<std::string_view>;
    { value.resource() } -> std::same_as<std::pmr::memory_resource*>;
};
template <typename entity_type>
consteval void validate_redis_entity() {
    static_assert(redis_entity_schema<entity_type>, "Redis repositories require a RUVIA_REDIS_ENTITY declaration");
    static_assert(redis_entity_columns_are_scalar<entity_type>(),
        "Redis repositories support only scalar Redis columns");
    static_assert(redis_entity_primary_key_count<entity_type>() == 1,
        "Redis entities must declare exactly one primary-key Redis column");
    static_assert(redis_entity_primary_key_is_valid<entity_type>(),
        "Redis primary keys must be non-nullable string or integer columns");
}

}  // namespace ruvia::detail
