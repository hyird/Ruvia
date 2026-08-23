#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia::detail {

class ContextSessionState;
struct SessionAccess;

}  // namespace ruvia::detail

namespace ruvia {

class Context;
class Next;

// A request-local capability bound by SessionMiddleware. The handle borrows
// Context state and therefore cannot escape the request.
class Session final {
public:
    [[nodiscard]] std::string_view data() const& noexcept;
    std::string_view data() const&& = delete;
    void set(std::string_view data);
    void clear();
    void regenerate();

private:
    explicit Session(detail::ContextSessionState& state) noexcept
        : state_(&state) {}

    detail::ContextSessionState* state_;
    friend class Context;
};

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/core/Task.h"
#include "ruvia/web/Middleware.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie, loads the blob at sess:<id> into the Context, runs the handler, then
// persists it with the configured TTL or deletes it if the handler changed the
// typed Session capability. A new session mints a random id in an HttpOnly
// cookie. The blob format is the application's; pair it with JSON if desired.
struct SessionConfig final {
    std::string redisAlias{"default"};
    std::string cookieName{"sid"};
    std::string keyPrefix{"sess:"};
    std::chrono::seconds ttl{std::chrono::hours(24)};
};

class SessionMiddleware final : public Middleware<SessionMiddleware> {
public:
    explicit SessionMiddleware(SessionConfig config = {});

    Task<void> handle(Context& c, Next& next);

private:
    SessionConfig config_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
