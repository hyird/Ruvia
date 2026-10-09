#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

namespace ruvia::detail {

template <typename T>
inline constexpr bool kDbOwningCharString = false;

template <typename Traits, typename Allocator>
inline constexpr bool kDbOwningCharString<std::basic_string<char, Traits, Allocator>> = true;

// One value the variadic query()/execute() overloads accept. A type that
// already denotes a whole parameter sequence -- std::span, std::array, or a
// vector of DbValue -- cannot construct a DbValue, so it fails this concept and
// the span overload keeps winning without needing an explicit exclusion.
template <typename Param>
concept DbParameter =
    std::constructible_from<DbValue, Param&&> || kDbOwningCharString<std::remove_cvref_t<Param>>;

template <typename... Params>
concept DbParameterPack = sizeof...(Params) > 0 && (DbParameter<Params> && ...);

// DB entry points clone each value into their own storage before returning.
// Borrow owned DbValue text and owning-string temporaries only across that
// synchronous boundary; DbValue itself still rejects retained string temporaries.
template <typename Param>
    requires DbParameter<Param>
[[nodiscard]] DbValue makeImmediateDbParameter(Param&& param) {
    if constexpr (std::same_as<std::remove_cvref_t<Param>, DbValue>) {
        if (DbValueAccess::type(param) == DbValueType::kString) {
            return DbValue(DbValueAccess::text(param));
        }
        return DbValue(std::forward<Param>(param));
    } else if constexpr (kDbOwningCharString<std::remove_cvref_t<Param>>) {
        return DbValue(std::string_view(param));
    } else {
        return DbValue(std::forward<Param>(param));
    }
}

}  // namespace ruvia::detail
