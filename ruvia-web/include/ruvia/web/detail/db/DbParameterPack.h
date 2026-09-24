#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/DbTypes.h"

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

// Variadic DB calls clone each value before returning. Convert an owning-string
// temporary to a view only inside that synchronous boundary; DbValue itself
// continues to reject such temporaries because it may otherwise be retained.
template <typename Param>
    requires DbParameter<Param>
[[nodiscard]] DbValue makeImmediateDbParameter(Param&& param) {
    if constexpr (kDbOwningCharString<std::remove_cvref_t<Param>>) {
        return DbValue(std::string_view(param));
    } else {
        return DbValue(std::forward<Param>(param));
    }
}

}  // namespace ruvia::detail
