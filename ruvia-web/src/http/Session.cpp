#include "ruvia/web/detail/http/SessionInternal.h"

#include "ruvia/web/Session.h"
#include "ruvia/web/detail/http/ContextInternal.h"

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/web/detail/http/CsrfInternal.h"
#include "ruvia/web/redis/RedisHandle.h"

#include <array>
#include <chrono>
#include <memory_resource>

namespace ruvia {

Task<void> SessionMiddleware::handle(Context& c, Next& next) {
    // Whether the client's `sid` was recognized -- i.e. an actual session blob
    // was found in the store under it. An id the client presents that is NOT in
    // the store must never be adopted (see the mint decision below).
    bool recognized = false;
    const auto cookie = c.req().cookie("sid");
    if (cookie && detail::isValidSessionId(*cookie)) {
        detail::SessionAccess::setId(c, *cookie);
        std::pmr::string key(c.resource());
        key.append("sess:");
        key.append(cookie->data(), cookie->size());
        if (auto stored = co_await c.redis("default").get(key)) {
            detail::SessionAccess::load(c, *stored);
            recognized = true;
        }
    }

    co_await next();

    if (detail::SessionAccess::dirty(c)) {
        auto& response = detail::ContextAccess::responseStorage(c);
        std::array<char, 64> idBuffer;
        auto id = detail::SessionAccess::id(c);
        // Mint a fresh id for a brand-new session AND whenever the client
        // presented an id that was not recognized (not found in the store).
        // Adopting an unrecognized client id would enable session fixation: an
        // attacker plants a known `sid`, then the victim authenticates and their
        // session is stored under the attacker-known id. A session that simply
        // expired out of the store is likewise renewed under a fresh id here.
        // regenerateSession() forces a fresh id even for a recognized session, so
        // an attacker who planted a known *recognized* id (their own live session)
        // cannot ride the victim's authenticated session after a privilege change.
        if (detail::sessionShouldMintNewId(
                id.empty(), recognized, detail::SessionAccess::regenerateRequested(c))) {
            // Regenerating a recognized session: drop the blob under the old id
            // first so the previous (possibly attacker-known) id no longer resolves.
            if (recognized && !id.empty()) {
                std::pmr::string oldKey(c.resource());
                oldKey.append("sess:");
                oldKey.append(id.data(), id.size());
                (void)(co_await c.redis("default").del(oldKey));
            }
            detail::SessionAccess::setId(c, detail::generateCsrfToken(idBuffer));
            id = detail::SessionAccess::id(c);
            if (id.empty()) {
                // CSPRNG failure: fail closed. Persisting under the empty id
                // would store every such session at the shared key "sess:",
                // and the client could never present a valid sid for it anyway.
                co_return;
            }
            const auto connection = getConnInfo(c);
            // The session id is only known after the handler ran, so the
            // cookie goes straight onto the already-built response rather
            // than through the context (whose headers were applied earlier).
            detail::appendSessionCookieHeader(
                response,
                c.resource(),
                id,
                connection.tls() != nullptr);
        }
        std::pmr::string key(c.resource());
        key.append("sess:");
        key.append(id.data(), id.size());
        const auto data = detail::SessionAccess::data(c);
        if (data.empty()) {
            (void)(co_await c.redis("default").del(key));
        } else {
            co_await c.redis("default").setEx(key, std::chrono::seconds(86400), data);
        }
    }
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
