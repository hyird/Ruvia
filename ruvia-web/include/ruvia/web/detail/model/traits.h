#pragma once

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/model_types.h"

// Internal layer. Users should include ruvia/web/model.h instead of this file.

namespace ruvia {
class json_object;
class json_value;
}  // namespace ruvia

namespace ruvia::detail {

// Shared model traits used by parser, validation rules, and generated macros.

enum class model_field_state : std::uint8_t { missing,
    parsed,
    null,
    invalid_type,
    duplicate };

template <typename>
inline constexpr bool always_false = false;

template <typename t_type>
inline constexpr bool is_ruvia_string = std::is_same_v<std::remove_cvref_t<t_type>, string>;

template <typename t_type>
inline constexpr bool is_ruvia_bytes = std::is_same_v<std::remove_cvref_t<t_type>, bytes>;

template <typename t_type>
inline constexpr bool is_ruvia_json_value = std::is_same_v<std::remove_cvref_t<t_type>, json_value>;

template <typename t_type>
inline constexpr bool is_ruvia_json_object = std::is_same_v<std::remove_cvref_t<t_type>, json_object>;

template <typename t_type>
struct ruvia_array_traits : std::false_type {};

template <typename value_t_type>
struct ruvia_array_traits<array<value_t_type>> : std::true_type {
    using value_type = value_t_type;
};

template <typename t_type>
inline constexpr bool is_ruvia_array = ruvia_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_boxed_array_traits : std::false_type {};

template <typename value_t_type>
struct ruvia_boxed_array_traits<boxed_array<value_t_type>> : std::true_type {
    using value_type = value_t_type;
};

template <typename t_type>
inline constexpr bool is_ruvia_boxed_array = ruvia_boxed_array_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
struct ruvia_scalar_traits : std::false_type {};

template <>
struct ruvia_scalar_traits<bool_value> : std::true_type {
    using value_type = bool;
};
template <typename value_t_type>
struct ruvia_scalar_traits<integer_value<value_t_type>> : std::true_type {
    using value_type = value_t_type;
};
template <>
struct ruvia_scalar_traits<float_value> : std::true_type {
    using value_type = float;
};
template <>
struct ruvia_scalar_traits<double_value> : std::true_type {
    using value_type = double;
};

template <typename t_type>
inline constexpr bool is_ruvia_scalar = ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value;

template <typename t_type>
using model_scalar_value_t_type = typename ruvia_scalar_traits<std::remove_cvref_t<t_type>>::value_type;

template <typename t_type>
inline constexpr bool is_form_field = is_ruvia_string<t_type> || is_ruvia_scalar<t_type>;

template <typename t_type>
inline constexpr bool is_model =
    requires { typename std::remove_cvref_t<t_type>::ruvia_model_schema_type; };

template <typename t_type>
inline constexpr bool model_field_value = is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> ||
                                          is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type> || is_model<t_type>;

template <typename value_t_type>
inline constexpr bool model_field_value<array<value_t_type>> = model_field_value<std::remove_cvref_t<value_t_type>>;

template <typename value_t_type>
inline constexpr bool model_field_value<boxed_array<value_t_type>> = model_field_value<std::remove_cvref_t<value_t_type>>;

template <typename t_type>
inline constexpr bool is_model_field = model_field_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
inline constexpr bool model_json_value =
    is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_scalar<t_type> || is_model<t_type>;

template <typename value_t_type>
inline constexpr bool model_json_value<array<value_t_type>> = model_json_value<std::remove_cvref_t<value_t_type>>;

template <typename value_t_type>
inline constexpr bool model_json_value<boxed_array<value_t_type>> = model_json_value<std::remove_cvref_t<value_t_type>>;

template <typename t_type>
inline constexpr bool is_model_json_value = model_json_value<std::remove_cvref_t<t_type>>;

template <typename t_type>
[[nodiscard]] t_type make_request_value(resolved_pmr_resource_tag, std::pmr::memory_resource* resource) {
    if constexpr (is_ruvia_string<t_type>) {
        return model_value_factory::make_string(resource);
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else if constexpr (is_ruvia_array<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else if constexpr (is_ruvia_boxed_array<t_type>) {
        return model_value_factory::make_boxed_array<t_type>(resource);
    } else if constexpr (is_model<t_type> || is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return t_type(model_options{.resource_ = resource});
    } else {
        (void)resource;
        return t_type{};
    }
}

template <typename t_type>
[[nodiscard]] t_type make_request_value(std::pmr::memory_resource* resource) {
    if constexpr (is_ruvia_string<t_type> || is_ruvia_bytes<t_type> || is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type> ||
                  is_model<t_type> || is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return make_request_value<t_type>(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource));
    } else {
        (void)resource;
        return t_type{};
    }
}

}  // namespace ruvia::detail
