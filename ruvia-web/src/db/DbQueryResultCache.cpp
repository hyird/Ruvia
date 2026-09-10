#include "ruvia/web/db/DbQueryResultCache.h"

#include <array>
#include <optional>
#include <utility>

#include "ruvia/web/detail/db/DbQueryCache.h"
#include "ruvia/web/detail/db/DbQueryCacheState.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia::detail {
#ifdef RUVIA_ENABLE_REDIS
namespace {
struct RedisCacheStore {
    RedisHandle redis;
    auto get(std::string_view key, OperationOptions options) {
        return redis.withOptions(std::move(options)).get(key);
    }
    auto put(std::string_view key, std::string_view value, std::chrono::milliseconds duration,
        OperationOptions options) {
        return redis.withOptions(std::move(options)).set(key, value, {.expiration = RedisSetExpiration::expiresAfter(duration)});
    }
};
Task<void> removeKeys(RedisHandle redis, std::pmr::vector<std::pmr::string> keys,
    OperationOptions options) {
    const OperationTimeout operationTimeout(options.timeout);
    for (const auto& key : keys) {
        try {
            co_await redis.withOptions(dbCacheRequiredOptions(options, operationTimeout)).del(key);
        } catch (const RedisError& error) {
            if (error.code() == RedisError::Code::kCancelled ||
                error.code() == RedisError::Code::kClosing) {
                throw;
            }
            if (operationTimeout.expired()) {
                throwDbCacheTimeout();
            }
            throw;
        }
        if (operationTimeout.expired()) {
            throwDbCacheTimeout();
        }
    }
}
Task<void> clearKeys(RedisHandle redis, std::pmr::string pattern, OperationOptions options) {
    const OperationTimeout operationTimeout(options.timeout);
    std::optional<RedisScanCursor> cursor;
    do {
        std::optional<RedisScanResult> result;
        try {
            result.emplace(co_await redis.withOptions(dbCacheRequiredOptions(options, operationTimeout)).scan({.cursor = cursor, .match = std::string_view(pattern), .count = 256}));
        } catch (const RedisError& error) {
            if (error.code() == RedisError::Code::kCancelled ||
                error.code() == RedisError::Code::kClosing) {
                throw;
            }
            if (operationTimeout.expired()) {
                throwDbCacheTimeout();
            }
            throw;
        }
        if (operationTimeout.expired()) {
            throwDbCacheTimeout();
        }
        for (const auto& key : result->values()) {
            try {
                co_await redis.withOptions(dbCacheRequiredOptions(options, operationTimeout)).del(key);
            } catch (const RedisError& error) {
                if (error.code() == RedisError::Code::kCancelled ||
                    error.code() == RedisError::Code::kClosing) {
                    throw;
                }
                if (operationTimeout.expired()) {
                    throwDbCacheTimeout();
                }
                throw;
            }
            if (operationTimeout.expired()) {
                throwDbCacheTimeout();
            }
        }
        cursor = result->nextCursor();
    } while (cursor);
}
}  // namespace
#endif
DbQueryCacheState::DbQueryCacheState(asio::io_context& io, const WorkerHandle& worker,
    const DbCacheConfigStorage& config, std::pmr::memory_resource* resource)
    : resource_(resource),
      duration_(config.duration),
      alwaysEnabled_(config.alwaysEnabled),
      ignoreErrors_(config.ignoreErrors),
      nameSpace_(config.nameSpace, resource) {
#ifdef RUVIA_ENABLE_REDIS
    const std::array definitions{RedisDefinition{std::pmr::string(kDefaultCapabilityAlias, resource), RedisConfigStorage(config.options, resource)}};
    redis_ = makePmrObject<RedisRegistry>(resource, io, resource, std::span<const RedisDefinition>(definitions), worker);
#else
    (void)io;
    (void)worker;
    throw std::invalid_argument("database caching requires Redis support");
#endif
}
DbQueryCacheState::~DbQueryCacheState() = default;
Task<void> DbQueryCacheState::connect() {
#ifdef RUVIA_ENABLE_REDIS
    co_await redis_->connect();
#endif
    co_return;
}
void DbQueryCacheState::closeNow() noexcept {
#ifdef RUVIA_ENABLE_REDIS
    redis_->closeNow();
#endif
}
std::optional<std::pmr::string> DbQueryCacheState::key(const DbQuery& query, const DbStatement& statement, DbDriver driver) {
    if (!query.cacheEnabled().value_or(alwaysEnabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return dbCacheKey(nameSpace_, query.cacheId(), statement.sql(), statement.params(), driver, resource_);
}
Task<DbRows> DbQueryCacheState::wrap(std::optional<std::chrono::milliseconds> duration, std::optional<std::pmr::string> key,
    DbCacheQuery database, ScopedOperationScope& scope, OperationOptions options,
    std::optional<OperationTimeout> deadline) {
#ifdef RUVIA_ENABLE_REDIS
    if (key) {
        return queryDbCache(RedisCacheStore{redis_->get(scope)}, std::move(*key),
            duration.value_or(duration_), ignoreErrors_, std::move(database), resource_,
            std::move(options), std::move(deadline));
    }
#else
    (void)duration;
    (void)key;
    (void)scope;
    (void)deadline;
#endif
    return std::move(database)(std::move(options));
}
Task<void> DbQueryCacheState::remove(std::span<const std::string_view> ids, ScopedOperationScope& scope, OperationOptions options) {
#ifdef RUVIA_ENABLE_REDIS
    std::pmr::vector<std::pmr::string> keys(resource_);
    keys.reserve(ids.size());
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
        keys.push_back(dbCacheKey(nameSpace_, id, {}, {}, DbDriver::kUnspecified, resource_));
    }
    return removeKeys(redis_->get(scope), std::move(keys), std::move(options));
#else
    (void)ids;
    (void)scope;
    (void)options;
    throw std::logic_error("Redis support is disabled");
#endif
}
Task<void> DbQueryCacheState::clear(ScopedOperationScope& scope, OperationOptions options) {
#ifdef RUVIA_ENABLE_REDIS
    auto pattern = dbCachePrefix(nameSpace_, resource_);
    pattern.push_back('*');
    return clearKeys(redis_->get(scope), std::move(pattern), std::move(options));
#else
    (void)scope;
    (void)options;
    throw std::logic_error("Redis support is disabled");
#endif
}
}  // namespace ruvia::detail

namespace ruvia {
DbQueryResultCache::DbQueryResultCache(detail::DbQueryCacheState& state, detail::ScopedOperationScope& scope, OperationOptions options) noexcept
    : detail::ScopedCapabilityNode(scope, &DbQueryResultCache::expireCapability),
      state_(&state),
      options_(std::move(options)) {}
void DbQueryResultCache::expireCapability(detail::ScopedCapabilityNode& node) noexcept {
    auto& cache = static_cast<DbQueryResultCache&>(node);
    cache.state_ = nullptr;
    cache.options_ = {};
}
ScopedOperation<void> DbQueryResultCache::remove(std::span<const std::string_view> ids) const {
    requireActive();
    return detail::makeScopedOperation(operationScope(), state_->remove(ids, operationScope(), options_));
}
ScopedOperation<void> DbQueryResultCache::clear() const {
    requireActive();
    return detail::makeScopedOperation(operationScope(), state_->clear(operationScope(), options_));
}
}  // namespace ruvia
