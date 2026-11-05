#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/operation_options.h"

namespace ruvia::detail {

// Classifies a raw pooled command before it is encoded. Connection-stateful
// commands are rejected; WAIT/WAITAOF are accepted only where a barrier is
// allowed. Returns whether the command needs the blocking pool; a blocking
// command throws when `allow_blocking` is false. An XREAD/XREADGROUP option
// this classifier does not know throws instead of being guessed non-blocking.
[[nodiscard]] bool validate_redis_pooled_command(
    std::span<const std::string_view> args, bool allow_blocking, bool allow_barrier = false);

// A wait the server may hold open indefinitely (a blocking command, a barrier,
// a subscription) must be bounded by its owner: a stop token or a finite
// operation timeout. Throws std::invalid_argument with `message` otherwise.
void require_bounded_redis_wait(const operation_options& options, const char* message);

}  // namespace ruvia::detail
