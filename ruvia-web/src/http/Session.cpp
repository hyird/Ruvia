#include "ruvia/web/detail/http/SessionAccess.h"

#include "ruvia/web/Session.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Next.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include <stdexcept>

namespace ruvia {

std::string_view Session::data() const& noexcept {
    return state_->data();
}

void Session::set(std::string_view data) {
    state_->set(data);
}

void Session::clear() {
    state_->clear();
}

void Session::regenerate() {
    state_->regenerate();
}

Session Context::session() {
    if (!sessionState_.available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return Session(sessionState_);
}

std::optional<Session> Context::trySession() noexcept {
    if (!sessionState_.available()) {
        return std::nullopt;
    }
    return Session(sessionState_);
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include "ruvia/web/detail/http/SecureToken.h"
#include "ruvia/web/redis/RedisHandle.h"
#include "ruvia/http/HttpHeader.h"

#include <array>
#include <chrono>
#include <memory_resource>

namespace ruvia {

SessionMiddleware::SessionMiddleware(SessionConfig config)
    : config_(std::move(config)) {
    if (config_.redisAlias.empty()) {
        throw std::invalid_argument("session Redis alias must not be empty");
    }
    if (!isValidHttpHeaderName(config_.cookieName)) {
        throw std::invalid_argument("session cookie name must be a valid HTTP token");
    }
    if (config_.keyPrefix.empty()) {
        throw std::invalid_argument("session key prefix must not be empty");
    }
    if (config_.ttl.count() <= 0) {
        throw std::invalid_argument("session TTL must be greater than zero");
    }
}

Task<void> SessionMiddleware::handle(Context& c, Next& next) {
    detail::SessionAccess::bind(c);
    const auto cookie = c.req().cookie(config_.cookieName);
    if (cookie && detail::isValidSessionId(*cookie)) {
        detail::SessionAccess::observePresentedId(c, *cookie);
        std::pmr::string key(c.resource());
        key.append(config_.keyPrefix);
        key.append(cookie->data(), cookie->size());
        if (auto stored = co_await c.redis(config_.redisAlias).get(key)) {
            detail::SessionAccess::load(c, *stored);
        }
    }

    co_await next();

    const auto& state = detail::SessionAccess::state(c);
    if (state.untouched() != nullptr || state.unrecognized() != nullptr || state.loaded() != nullptr) {
        co_return;
    }

    const auto connection = getConnInfo(c);
    const bool secure = connection.scheme() == HttpScheme::kHttps;
    if (const auto* cleared = state.cleared()) {
        if (cleared->oldId.has_value()) {
            std::pmr::string key(c.resource());
            key.append(config_.keyPrefix);
            key.append(cleared->oldId->data(), cleared->oldId->size());
            (void)(co_await c.redis(config_.redisAlias).del(key));
        }
        auto& response = detail::ContextAccess::responseStorage(c);
        detail::appendExpiredSessionCookieHeader(response, c.resource(), config_.cookieName, secure);
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
            c.respond(c.error({.status = ruvia::http_status::kInternalServerError, .code = "secure_random_failed", .message = "secure token generation failed"}));
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
                key.append(config_.keyPrefix);
                key.append(existingId.data(), existingId.size());
                ruvia::RedisSetOptions options;
                options.expiration = ruvia::RedisSetExpiration::expiresAfter(config_.ttl);
                (void)(co_await c.redis(config_.redisAlias).set(key, data, std::move(options)));
                break;
            }
            case detail::SessionCommitStep::kDeleteOld: {
                std::pmr::string oldKey(c.resource());
                oldKey.append(config_.keyPrefix);
                oldKey.append(oldIdToDelete.data(), oldIdToDelete.size());
                (void)(co_await c.redis(config_.redisAlias).del(oldKey));
                break;
            }
            case detail::SessionCommitStep::kPublishCurrentCookie: {
                auto& response = detail::ContextAccess::responseStorage(c);
                detail::appendSessionCookieHeader(response, c.resource(), config_.cookieName, existingId, secure);
                break;
            }
        }
    }
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
