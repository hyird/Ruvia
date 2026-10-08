#include "ruvia/web/Session.h"

#include <stdexcept>

#include "ruvia/web/Context.h"
#include "ruvia/web/Next.h"

#include "context/ContextAccess.h"
#include "http/SessionAccess.h"

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
    if (!sessionState().available()) {
        throw std::logic_error("session capability is not bound for this request");
    }
    return Session(sessionState());
}

std::optional<Session> Context::trySession() noexcept {
    if (!sessionState().available()) {
        return std::nullopt;
    }
    return Session(sessionState());
}

Task<void> detail::SessionAccess::commit(Context& context) {
    auto& state = context.sessionState();
    if (!state.beginCommit()) {
        co_return;
    }
    try {
#ifdef RUVIA_ENABLE_REDIS
        if (const auto* owner = state.owner()) {
            co_await owner->commit(context);
        }
#endif
        state.finishCommit();
    } catch (...) {
        state.failCommit(std::current_exception());
        throw;
    }
}

}  // namespace ruvia

#ifdef RUVIA_ENABLE_REDIS

#include <array>
#include <charconv>
#include <chrono>
#include <memory_resource>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/detail/util/RegistrationResource.h"
#include "ruvia/web/redis/RedisHandle.h"

#include "http/SecureToken.h"

namespace ruvia {

SessionMiddleware::ConfigStorage::ValidatedConfig SessionMiddleware::ConfigStorage::validate(
    const SessionConfig& source) {
    if (source.redisAlias.empty()) {
        throw std::invalid_argument("session Redis alias must not be empty");
    }
    if (!isValidHttpHeaderName(source.cookieName)) {
        throw std::invalid_argument("session cookie name must be a valid HTTP token");
    }
    if (source.keyPrefix.empty()) {
        throw std::invalid_argument("session key prefix must not be empty");
    }
    if (source.ttl.count() <= 0) {
        throw std::invalid_argument("session TTL must be greater than zero");
    }
    return ValidatedConfig{.source = &source};
}

SessionMiddleware::ConfigStorage::ConfigStorage(
    const SessionConfig& source, std::pmr::memory_resource* resource)
    : ConfigStorage(validate(source), resource) {}

SessionMiddleware::ConfigStorage::ConfigStorage(
    ValidatedConfig validated, std::pmr::memory_resource* resource)
    : redisAlias(validated.source->redisAlias, resource),
      cookieName(validated.source->cookieName, resource),
      keyPrefix(validated.source->keyPrefix, resource),
      ttl(validated.source->ttl) {}

SessionMiddleware::SessionMiddleware()
    : SessionMiddleware(SessionConfig{}) {}

SessionMiddleware::SessionMiddleware(const SessionConfig& config)
    : config_(config, detail::registrationResource()) {}

Task<void> SessionMiddleware::handle(Context& c, Next& next) {
    detail::SessionAccess::bind(c, this);
    const auto cookie = c.req().cookie(config_.cookieName);
    if (cookie && detail::isValidSessionId(*cookie)) {
        detail::SessionAccess::observePresentedId(c, *cookie);
        std::pmr::string key(c.pool());
        key.append(config_.keyPrefix);
        key.append(cookie->data(), cookie->size());
        if (auto stored = co_await c.redis(config_.redisAlias).get(key)) {
            detail::SessionAccess::load(c, *stored);
        }
    }

    co_await next();
    co_await detail::SessionAccess::commit(c);
}

Task<void> SessionMiddleware::commit(Context& c) const {
    const auto& state = detail::SessionAccess::state(c);
    if (state.untouched() != nullptr || state.unrecognized() != nullptr ||
        state.loaded() != nullptr) {
        co_return;
    }

    const auto connection = c.conn();
    const bool secure = connection.scheme() == HttpScheme::kHttps;
    if (const auto* cleared = state.cleared()) {
        auto& response = detail::ContextAccess::responseStorage(c);
        auto staged = response.cloneHeadersForTransaction(1);
        detail::appendExpiredSessionCookieHeader(staged, c.pool(), config_.cookieName, secure);
        if (cleared->oldId.has_value()) {
            std::pmr::string key(c.pool());
            key.append(config_.keyPrefix);
            key.append(cleared->oldId->data(), cleared->oldId->size());
            (void)(co_await c.redis(config_.redisAlias).del(key));
        }
        response.commitHeadersFrom(std::move(staged));
        co_return;
    }

    std::string_view data;
    std::string_view old_id;
    if (const auto* rotated = state.rotate()) {
        data = rotated->data;
        old_id = rotated->oldId;
    } else {
        data = state.persistNew()->data;
    }

    std::array<char, 64> id_buffer;
    const auto token_result = detail::generateSecureToken(id_buffer);
    const auto* token = token_result.ready();
    if (token == nullptr) {
        throw HttpError({.status = ruvia::http_status::kInternalServerError,
            .code = "secure_random_failed",
            .message = "secure token generation failed"});
    }
    const auto new_id = token->value();

    // Allocate and validate the cookie before changing Redis. Publication after
    // successful storage is an allocation-free move of the complete header state.
    auto& response = detail::ContextAccess::responseStorage(c);
    auto staged = response.cloneHeadersForTransaction(1);
    detail::appendSessionCookieHeader(staged, c.pool(), config_.cookieName, new_id, secure);

    std::pmr::string key(c.pool());
    key.append(config_.keyPrefix).append(new_id);
    bool applied = false;
    if (!old_id.empty()) {
        std::pmr::string old_key(c.pool());
        old_key.append(config_.keyPrefix).append(old_id);
        std::array<char, 32> ttl_buffer{};
        const auto [ttl_end, ttl_error] = std::to_chars(ttl_buffer.data(), ttl_buffer.data() + ttl_buffer.size(), config_.ttl.count());
        if (ttl_error != std::errc{}) {
            throw std::logic_error("session TTL serialization failed");
        }
        const std::array<std::string_view, 2> keys{old_key, key};
        const std::array<std::string_view, 2> args{data, std::string_view(ttl_buffer.data(), ttl_end)};
        // Redis executes the entire transition atomically. A stale request may
        // neither recreate a revoked identifier nor mint a successor from it.
        const auto result = co_await c.redis(config_.redisAlias).eval(detail::session_rotation_script, keys, args);
        if (result.kind() == RedisValue::Kind::kError) {
            throw RedisError(RedisError::Code::kCommandError, result.error());
        }
        applied = result.integer() == 1;
    } else {
        RedisSetOptions options;
        options.condition = RedisSetCondition::kIfAbsent;
        options.expiration = RedisSetExpiration::expiresAfter(config_.ttl);
        const auto result = co_await c.redis(config_.redisAlias).set(key, data, std::move(options));
        applied = result.applied();
    }
    if (!applied) {
        throw HttpError({.status = http_status::kConflict,
            .code = "session_conflict",
            .message = "session expired or was revoked; retry with a new session"});
    }
    response.commitHeadersFrom(std::move(staged));
}

}  // namespace ruvia

#endif  // RUVIA_ENABLE_REDIS
