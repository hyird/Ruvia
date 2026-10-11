#pragma once

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

using route_handler_type = callback_ref_type<task<http_response>(context&)>;
using route_stream_handler_type = callback_ref_type<task<void>(context&)>;
using route_middleware_type = callback_ref_type<task<void>(context&, next&)>;

}  // namespace detail

}  // namespace ruvia
