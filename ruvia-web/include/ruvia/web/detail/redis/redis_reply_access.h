#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

// Typed reply accessors that enforce the redis_value kind. A reply's type is
// chosen by the (untrusted) server, so a mismatch throws
// redis_error(protocol_error) rather than the raw accessors' std::logic_error,
// and an error reply throws redis_error(command_error).
[[nodiscard]] std::string_view redis_value_string(const redis_value& value);
[[nodiscard]] std::int64_t redis_value_integer(const redis_value& value);
[[nodiscard]] std::span<const redis_value> redis_value_array(const redis_value& value);
void throw_if_redis_error(const redis_value& value);

}  // namespace ruvia::detail
