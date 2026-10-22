#pragma once

#include <cstdint>

#include "ruvia/core/task.h"
#include "ruvia/web/detail/callback_ref.h"

// The three non-owning callable shapes a route runs: a buffered handler, a
// streaming (response-stream / websocket / tunnel) handler, and one middleware
// frame. Each binds a worker-local instance to a process-invariant invoke thunk.

namespace ruvia {

class context;
class http_response;
class next;

namespace detail {

enum class request_body_mode : std::uint8_t { buffered,
    stream };

using route_handler = callback_ref<task<http_response>(context&)>;
using route_stream_handler = callback_ref<task<void>(context&)>;
using route_middleware = callback_ref<task<void>(context&, next&)>;

}  // namespace detail

}  // namespace ruvia
