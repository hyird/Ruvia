#pragma once

#include <cstddef>

#include "ruvia/core/task.h"
#include "ruvia/web/context.h"
#include "ruvia/web/detail/middleware/middleware_registration.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/next.h"

namespace ruvia {

// A request-body ceiling for the routes it is registered on:
//
//     RUVIA_POST("/avatar", upload, ruvia::body_limit<256 * 1024>);
//
// server_config::max_buffered_body_bytes has to be sized for the largest body any
// route accepts,
// which leaves every other route accepting that much too. This is the same
// policy at a narrower scope: an endpoint expecting a small JSON document stops
// reading at a small JSON document.
//
// One rule governs every policy that has both an app-wide and a route-level
// form: the narrower scope may only TIGHTEN. A route cannot lift the
// deployment-wide bound, and where a controller-wide and a route-specific
// declaration both exist the stricter wins rather than the nearer. On a stream
// route this also gives an otherwise unbounded body a bound.
//
// This is a declaration the server reads BEFORE the body is accepted, not code
// that runs in the chain -- by the time a middleware's handle() runs, the bytes
// it would have rejected are already buffered. Registering it on a controller
// applies it to that controller's routes, and where both a controller-wide and
// a route-specific one exist the stricter wins.
template <std::size_t max_bytes>
class body_limit final : public middleware {
public:
    static_assert(max_bytes > 0, "body limit must be greater than 0");

    static constexpr std::size_t ruvia_request_body_limit_bytes = max_bytes;

    task<void> handle(context&, next& next_value) {
        co_await next_value();
    }
};

}  // namespace ruvia
