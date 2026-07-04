#include "SessionInternal.h"

#include "ruvia/http/Session.h"

#ifdef RUVIA_ENABLE_REDIS

#include "CsrfInternal.h"
#include "ruvia/redis/RedisHandle.h"

#include <array>
#include <chrono>
#include <memory_resource>

namespace ruvia {

Task<void> SessionMiddleware::handle(Context& c, Next& next) {
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
            detail::appendSessionCookieHeader(response, c.resource(), id, c.req().raw().isSecure());
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

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
