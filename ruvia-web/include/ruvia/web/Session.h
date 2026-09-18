#pragma once

#include <chrono>
#include <memory_resource>
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
// Changes are persisted automatically before the response head (including a
// stream or WebSocket handshake) is sent. Once that commit begins, set(),
// clear(), and regenerate() throw std::logic_error; data() remains readable.
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
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie and loads the blob at sess:<id> into the Context. Before the response
// head is sent, it persists changes with the configured TTL or deletes the
// session. A new session mints a random id in an HttpOnly
// cookie. The blob format is the application's; pair it with JSON if desired.
struct SessionConfig final {
    std::string redisAlias{"default"};
    std::string cookieName{"sid"};
    std::string keyPrefix{"sess:"};
    std::chrono::seconds ttl{std::chrono::hours(24)};
};

class SessionMiddleware final : public Middleware {
public:
    SessionMiddleware();
    explicit SessionMiddleware(const SessionConfig& config);

    SessionMiddleware(const SessionMiddleware&) = delete;
    SessionMiddleware& operator=(const SessionMiddleware&) = delete;
    SessionMiddleware(SessionMiddleware&&) = delete;
    SessionMiddleware& operator=(SessionMiddleware&&) = delete;

    Task<void> handle(Context& c, Next& next);

private:
    friend struct detail::SessionAccess;
    Task<void> commit(Context& c) const;

    struct ConfigStorage final {
        ConfigStorage(const SessionConfig& source, std::pmr::memory_resource* resource);

        std::pmr::string redisAlias;
        std::pmr::string cookieName;
        std::pmr::string keyPrefix;
        std::chrono::seconds ttl;

    private:
        struct ValidatedConfig final {
            const SessionConfig* source;
        };

        [[nodiscard]] static ValidatedConfig validate(const SessionConfig& source);
        ConfigStorage(ValidatedConfig validated, std::pmr::memory_resource* resource);
    };

    ConfigStorage config_;
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
