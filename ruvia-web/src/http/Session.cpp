#include "ruvia/web/detail/http/SessionAccess.h"

#include "ruvia/web/Session.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

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
        if (auto stored = co_await c.redis(redisAlias_.view()).get(key)) {
            detail::SessionAccess::load(c, *stored);
        }
    }

    co_await next();

    const auto& state = detail::SessionAccess::state(c);
    if (state.untouched() != nullptr || state.unrecognized() != nullptr || state.loaded() != nullptr) {
        co_return;
    }

    const auto connection = getConnInfo(c);
    const bool secure = connection.secure();
    if (const auto* cleared = state.cleared()) {
        if (cleared->oldId.has_value()) {
            std::pmr::string key(c.resource());
            key.append("sess:");
            key.append(cleared->oldId->data(), cleared->oldId->size());
            (void)(co_await c.redis(redisAlias_.view()).del(key));
        }
        auto& response = detail::ContextAccess::responseStorage(c);
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
    }

    // Do not publish a newly minted id until its blob has been persisted, and do
    // not destroy a recognized session until its replacement exists. An RNG,
    // Redis SET, or Redis DEL failure leaves the client's previous cookie
    // untouched instead of returning a Set-Cookie for a missing session.
    const auto commitPlan = detail::sessionCommitPlan(existingId, oldIdToDelete, mintNewId);
    for (std::size_t i = 0; i < commitPlan.count; ++i) {
        switch (commitPlan.steps[i]) {
            case detail::SessionCommitStep::kPersistCurrent: {
                std::pmr::string key(c.resource());
                key.append("sess:");
                key.append(existingId.data(), existingId.size());
                ruvia::RedisSetOptions options;
                options.expiration = ruvia::RedisSetExpiration::expiresAfter(std::chrono::hours(24));
                (void)(co_await c.redis(redisAlias_.view()).set(key, data, std::move(options)));
                break;
            }
            case detail::SessionCommitStep::kDeleteOld: {
                std::pmr::string oldKey(c.resource());
                oldKey.append("sess:");
                oldKey.append(oldIdToDelete.data(), oldIdToDelete.size());
                (void)(co_await c.redis(redisAlias_.view()).del(oldKey));
                break;
            }
            case detail::SessionCommitStep::kPublishCurrentCookie: {
                auto& response = detail::ContextAccess::responseStorage(c);
                detail::appendSessionCookieHeader(response, c.resource(), existingId, secure);
                break;
            }
        }
    }
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
