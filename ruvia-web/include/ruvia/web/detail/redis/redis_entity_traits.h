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
template <typename t_type>
inline constexpr bool is_redis_entity_string =
    std::is_same_v<std::remove_cvref_t<t_type>, string> ||
    std::is_same_v<std::remove_cvref_t<t_type>, std::pmr::string>;

template <typename t_type>
inline constexpr bool is_redis_entity_model_scalar =
    is_ruvia_scalar<std::remove_cvref_t<t_type>>;

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

template <typename t_type, bool = is_redis_entity_model_scalar<t_type>>
struct redis_entity_model_integer final : std::false_type {};

template <typename t_type>
struct redis_entity_model_integer<t_type, true> final
    : std::bool_constant<
          std::is_integral_v<model_scalar_value_t_type<std::remove_cvref_t<t_type>>> &&
          !std::is_same_v<model_scalar_value_t_type<std::remove_cvref_t<t_type>>, bool>> {};

template <typename t_type>
inline constexpr bool is_redis_entity_model_integer = redis_entity_model_integer<t_type>::value;

template <typename t_type>
inline constexpr bool is_redis_entity_primary_key_scalar =
    is_redis_entity_string<t_type> || is_redis_entity_native_integer<t_type> || is_redis_entity_model_integer<t_type>;

template <typename t_type>
inline constexpr bool is_redis_entity_number =
    is_redis_entity_native_number<t_type> ||
    (is_redis_entity_model_scalar<t_type> && !std::is_same_v<std::remove_cvref_t<t_type>, bool_value>);

template <typename t_type>
inline constexpr bool is_redis_entity_scalar =
    is_redis_entity_string<t_type> || std::is_same_v<std::remove_cvref_t<t_type>, bool> ||
    is_redis_entity_native_number<t_type> || is_redis_entity_model_scalar<t_type>;

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
