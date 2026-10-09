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

struct redis_entity_field_options final {
    bool id_{false};
    bool nullable_{false};
    constexpr bool operator==(const redis_entity_field_options&) const = default;
};

template <typename column_t_type>
struct redis_entity_field_adapter final {
    static constexpr auto name = column_t_type::name;
    using value_type = typename column_t_type::value_type;
    using column_type = column_t_type;
    static constexpr redis_entity_field_options options{
        .id_ = column_type::options.primary_key_,
        .nullable_ = column_type::options.nullable_,
    };
};

template <typename entity_type, std::size_t... i>
consteval bool redis_entity_columns_are_scalar(std::index_sequence<i...>) {
    return (is_redis_entity_scalar<
                typename std::tuple_element_t<i, typename entity_type::columns_type>::value_type> &&
            ...);
}

template <typename entity_type>
consteval bool redis_entity_columns_are_scalar() {
    return redis_entity_columns_are_scalar<entity_type>(
        std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
}

template <typename entity_type, std::size_t... i>
consteval std::size_t redis_entity_primary_key_count(std::index_sequence<i...>) {
    return (std::size_t{
                std::tuple_element_t<i, typename entity_type::columns_type>::options.primary_key_} +
            ... +
            std::size_t{0});
}

template <typename entity_type>
consteval std::size_t redis_entity_primary_key_count() {
    return redis_entity_primary_key_count<entity_type>(
        std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
}

template <typename entity_type, std::size_t... i>
consteval bool redis_entity_primary_key_is_valid(std::index_sequence<i...>) {
    return ((
                !std::tuple_element_t<i, typename entity_type::columns_type>::options.primary_key_ ||
                (is_redis_entity_primary_key_scalar<typename std::tuple_element_t<
                        i, typename entity_type::columns_type>::value_type> &&
                    !std::tuple_element_t<i, typename entity_type::columns_type>::options.nullable_)) &&
            ...);
}

template <typename entity_type>
consteval bool redis_entity_primary_key_is_valid() {
    return redis_entity_primary_key_is_valid<entity_type>(
        std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
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
