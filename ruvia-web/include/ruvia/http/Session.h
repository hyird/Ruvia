#pragma once

#include <string_view>

#include "ruvia/http/Context.h"

namespace ruvia::detail {

struct SessionAccess;

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/app/Task.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie, loads the blob at sess:<id> into the Context, runs the handler, then
// persists (SETEX, 1-day TTL) or deletes it if the handler changed the session
// via c.setSession()/c.clearSession(). A new session mints a random id (HttpOnly
// cookie). The blob format is the application's; pair it with JSON if desired.
// Uses the "default" Redis connection.
class SessionMiddleware final : public Middleware<SessionMiddleware> {
public:
    Task<void> handle(Context& c, Next& next);
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
