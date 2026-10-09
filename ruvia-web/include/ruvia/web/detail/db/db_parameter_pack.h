#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {

template <typename t_type>
inline constexpr bool db_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool db_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

// One value the variadic query()/execute() overloads accept. A type that
// already denotes a whole parameter sequence -- std::span, std::array, or a
// vector of db_value -- cannot construct a db_value, so it fails this concept and
// the span overload keeps winning without needing an explicit exclusion.
template <typename param_type>
concept db_parameter =
    std::constructible_from<db_value, param_type&&> || db_owning_char_string<std::remove_cvref_t<param_type>>;

template <typename... params_type>
concept db_parameter_pack = sizeof...(params_type) > 0 && (db_parameter<params_type> && ...);

// DB entry points clone each value into their own storage before returning.
// Borrow owned db_value text and owning-string temporaries only across that
// synchronous boundary; db_value itself still rejects retained string temporaries.
template <typename param_type>
    requires db_parameter<param_type>
[[nodiscard]] db_value make_immediate_db_parameter(param_type&& param) {
    if constexpr (std::same_as<std::remove_cvref_t<param_type>, db_value>) {
        if (db_value_access::type(param) == db_value_type::string) {
            return db_value(db_value_access::text(param));
        }
        return db_value(std::forward<param_type>(param));
    } else if constexpr (db_owning_char_string<std::remove_cvref_t<param_type>>) {
        return db_value(std::string_view(param));
    } else {
        return db_value(std::forward<param_type>(param));
    }
}

}  // namespace ruvia::detail
