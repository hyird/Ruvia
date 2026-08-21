#pragma once

#include <string_view>

#include "ruvia/web/Context.h"

namespace ruvia::detail {

struct SessionAccess;

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/core/Task.h"
#include "ruvia/http/BorrowedText.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie, loads the blob at sess:<id> into the Context, runs the handler, then
// persists (SETEX, 1-day TTL) or deletes it if the handler changed the session
// via c.setSession()/c.clearSession(). A new session mints a random id (HttpOnly
// cookie). The blob format is the application's; pair it with JSON if desired.
struct SessionMiddlewareOptions final {
    ::ruvia::BorrowedText redisAlias{"default"};
};

class SessionMiddleware final : public Middleware<SessionMiddleware> {
public:
    explicit SessionMiddleware(SessionMiddlewareOptions options = {}) noexcept
        : redisAlias_(options.redisAlias) {}

    Task<void> handle(Context& c, Next& next);

private:
    ::ruvia::BorrowedText redisAlias_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
