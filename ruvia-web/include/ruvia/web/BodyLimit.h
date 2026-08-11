#pragma once

#include <cstddef>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

// A request-body ceiling for the routes it is registered on:
//
//     RUVIA_POST("/avatar", upload, ruvia::BodyLimit<256 * 1024>);
//
// App::setBodyLimit() has to be sized for the largest body any route accepts,
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
template <std::size_t MaxBytes>
class BodyLimit final : public Middleware<BodyLimit<MaxBytes>> {
public:
    static_assert(MaxBytes > 0, "body limit must be greater than 0");

    static constexpr std::size_t ruviaRequestBodyLimitBytes = MaxBytes;

    Task<void> handle(Context&, Next& next) {
        co_await next();
    }
};

}  // namespace ruvia
