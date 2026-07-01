#pragma once

#include <string_view>

#include "ruvia/http/Context.h"

namespace ruvia::detail {

// Privileged access to a Context's session slot, used by the session middleware
// to load the stored blob in and read what the handler left behind.
struct SessionAccess final {
    static void setId(Context& context, std::string_view id) {
        context.sessionIdStorage().assign(id.data(), id.size());
    }
    static void load(Context& context, std::string_view data) {
        assignStableString(context.sessionDataStorage(), data);
    }
    [[nodiscard]] static bool dirty(const Context& context) noexcept {
        return context.sessionDirty_;
    }
    [[nodiscard]] static std::string_view id(const Context& context) noexcept {
        return context.sessionId();
    }
    [[nodiscard]] static std::string_view data(const Context& context) noexcept {
        return context.session();
    }
};

[[nodiscard]] inline bool isValidSessionId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia::detail

#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <chrono>
#include <memory_resource>

#include "ruvia/app/Task.h"
#include "ruvia/http/Cookies.h"
#include "ruvia/http/Csrf.h"
#include "ruvia/http/MiddlewareRuntime.h"
#include "ruvia/http/Next.h"
#include "ruvia/redis/RedisHandle.h"

namespace ruvia {

// Server-side session backed by Redis (RUVIA_ENABLE_REDIS). Reads the `sid`
// cookie, loads the blob at sess:<id> into the Context, runs the handler, then
// persists (SETEX, 1-day TTL) or deletes it if the handler changed the session
// via c.setSession()/c.clearSession(). A new session mints a random id (HttpOnly
// cookie). The blob format is the application's; pair it with JSON if desired.
// Uses the "default" Redis connection.
class SessionMiddleware final : public Middleware<SessionMiddleware> {
public:
    Task<void> handle(Context& c, Next next) {
        const auto cookie = c.req().cookie("sid");
        if (cookie && detail::isValidSessionId(*cookie)) {
            detail::SessionAccess::setId(c, *cookie);
            std::pmr::string key(c.resource());
            key.append("sess:");
            key.append(cookie->data(), cookie->size());
            if (auto stored = co_await c.redis("default").get(key)) {
                detail::SessionAccess::load(c, *stored);
            }
        }

        co_await next();

        if (detail::SessionAccess::dirty(c)) {
            auto& response = c.res();
            std::array<char, 64> idBuffer;
            auto id = detail::SessionAccess::id(c);
            if (id.empty()) {
                detail::SessionAccess::setId(c, detail::generateCsrfToken(idBuffer));
                id = detail::SessionAccess::id(c);
                // The session id is only known after the handler ran, so the
                // cookie goes straight onto the already-built response rather
                // than through the context (whose headers were applied earlier).
                std::pmr::string setCookie(c.resource());
                setCookie.append("sid=");
                setCookie.append(id.data(), id.size());
                setCookie.append("; Path=/; HttpOnly; SameSite=Lax");
                if (c.req().isSecure()) {
                    setCookie.append("; Secure");
                }
                response.setHeader("Set-Cookie", setCookie);
            }
            std::pmr::string key(c.resource());
            key.append("sess:");
            key.append(id.data(), id.size());
            const auto data = detail::SessionAccess::data(c);
            if (data.empty()) {
                co_await c.redis("default").del(key);
            } else {
                co_await c.redis("default").setEx(key, std::chrono::seconds(86400), data);
            }
        }
    }
};

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
