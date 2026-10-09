#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>

namespace ruvia::detail {

// One textual argument the variadic Redis entry points accept. A span of
// string_view is not itself convertible to string_view, so a caller that already
// holds a contiguous sequence still selects the span overload.
//
// Variadic entry points synchronously clone every view into operation- or
// batch-owned storage, so owning-string temporaries remain alive long enough.
// Explicit span/view APIs keep their existing caller-owned lifetime contract.
template <typename arg_type>
concept redis_argument = std::convertible_to<arg_type&&, std::string_view>;

template <typename... args_type>
concept redis_argument_pack = sizeof...(args_type) > 0 && (redis_argument<args_type> && ...);

// Commands that take alternating name/value arguments (MSET, HSET). An odd
// count would leave a trailing name with no value, which Redis rejects at the
// wire level; requiring an even count turns that into a compile error.
template <typename... args_type>
concept redis_pair_argument_pack = redis_argument_pack<args_type...> && sizeof...(args_type) % 2 == 0;

}  // namespace ruvia::detail
