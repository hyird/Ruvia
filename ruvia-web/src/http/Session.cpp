#include "ruvia/web/detail/http/SessionAccess.h"

#include "ruvia/web/Session.h"
#include "ruvia/web/detail/http/ContextAccess.h"

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/web/detail/http/SecureToken.h"
#include "ruvia/web/redis/RedisHandle.h"

#include <array>
#include <chrono>
#include <memory_resource>

namespace ruvia {

Task<void> SessionMiddleware::handle(Context& c, Next& next) {
    const auto cookie = c.req().cookie("sid");
    if (cookie && detail::isValidSessionId(*cookie)) {
        detail::SessionAccess::observePresentedId(c, *cookie);
        std::pmr::string key(c.resource());
        key.append("sess:");
        key.append(cookie->data(), cookie->size());
        if (auto stored = co_await c.redis("default").get(key)) {
            detail::SessionAccess::load(c, *stored);
        }
    }

    co_await next();

    const auto& state = detail::SessionAccess::state(c);
    if (state.untouched() != nullptr || state.unrecognized() != nullptr ||
        state.loaded() != nullptr) {
        co_return;
    }

    auto& response = detail::ContextAccess::responseStorage(c);
    const auto connection = getConnInfo(c);
    const bool secure = connection.tls() != nullptr;
    if (const auto* cleared = state.cleared()) {
        if (cleared->oldId.has_value()) {
            std::pmr::string key(c.resource());
            key.append("sess:");
            key.append(cleared->oldId->data(), cleared->oldId->size());
            (void)(co_await c.redis("default").del(key));
        }
        detail::appendExpiredSessionCookieHeader(response, c.resource(), secure);
        co_return;
    }

    std::string_view data;
    std::string_view existingId;
    std::string_view oldIdToDelete;
    bool mintNewId = false;
    if (const auto* fresh = state.persistNew()) {
        data = fresh->data;
        mintNewId = true;
    } else if (const auto* existing = state.persistExisting()) {
        data = existing->data;
        existingId = existing->id;
    } else if (const auto* rotated = state.rotate()) {
        data = rotated->data;
        oldIdToDelete = rotated->oldId;
        mintNewId = true;
    }

    std::array<char, 64> idBuffer;
    if (mintNewId) {
        const auto tokenResult = detail::generateSecureToken(idBuffer);
        const auto* token = tokenResult.ready();
        if (token == nullptr) {
            c.respond(c.error(ruvia::http_status::kInternalServerError, "secure_random_failed", "secure token generation failed"));
            co_return;
        }
        existingId = token->value();
        detail::appendSessionCookieHeader(response, c.resource(), existingId, secure);
    }

    // Do not destroy a recognized session until a replacement id exists. An
    // RNG failure returns 500 above while leaving the old session usable.
    if (!oldIdToDelete.empty()) {
        std::pmr::string oldKey(c.resource());
        oldKey.append("sess:");
        oldKey.append(oldIdToDelete.data(), oldIdToDelete.size());
        (void)(co_await c.redis("default").del(oldKey));
    }

    if (!existingId.empty()) {
        std::pmr::string key(c.resource());
        key.append("sess:");
        key.append(existingId.data(), existingId.size());
        co_await c.redis("default").setEx(key, std::chrono::seconds(86400), data);
    }
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
