#pragma once

#include <concepts>

#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/web/db/DbTypes.h"

namespace ruvia::detail {

// One value the variadic query()/execute() overloads accept. A type that
// already denotes a whole parameter sequence -- std::span, std::array, or a
// vector of DbValue -- cannot construct a DbValue, so it fails this concept and
// the span overload keeps winning without needing an explicit exclusion.
template <typename Param>
concept DbParameter = std::constructible_from<DbValue, Param&&>;

template <typename... Params>
concept DbParameterPack = sizeof...(Params) > 0 && (DbParameter<Params> && ...);

// An owning-string temporary would leave DbValue's borrowed text dangling.
// DbValue already deletes those constructors, which alone would reduce the call
// to "no matching overload"; naming the case lets the variadic entry points
// delete a matching overload instead, so the diagnostic points at the argument.
template <typename... Params>
concept DbTemporaryOwningParameterPack = sizeof...(Params) > 0 && (HttpTemporaryOwningCharString<Params> || ...);

}  // namespace ruvia::detail
