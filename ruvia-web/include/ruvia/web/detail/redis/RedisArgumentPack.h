#pragma once

#include <concepts>
#include <cstddef>

#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

// One textual argument the variadic Redis entry points accept. A span of
// string_view is not itself convertible to string_view, so a caller that already
// holds a contiguous sequence still selects the span overload.
//
// Owning-string temporaries are excluded here rather than accepted and deleted
// later: unlike DbValue, std::string_view has no deleted constructor to lean on,
// so admitting them would silently borrow text that dies with the argument.
template <typename Arg>
concept RedisArgument = std::convertible_to<Arg&&, std::string_view> && !HttpTemporaryOwningCharString<Arg>;

template <typename... Args>
concept RedisArgumentPack = sizeof...(Args) > 0 && (RedisArgument<Args> && ...);

// Commands that take alternating name/value arguments (MSET, HSET). An odd
// count would leave a trailing name with no value, which Redis rejects at the
// wire level; requiring an even count turns that into a compile error.
template <typename... Args>
concept RedisPairArgumentPack = RedisArgumentPack<Args...> && sizeof...(Args) % 2 == 0;

template <typename... Args>
concept RedisTemporaryOwningArgumentPack = sizeof...(Args) > 0 && (HttpTemporaryOwningCharString<Args> || ...);

}  // namespace ruvia::detail
