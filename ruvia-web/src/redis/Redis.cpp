#include "ruvia/web/redis/Redis.h"

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/redis/RedisInternal.h"
#include "ruvia/web/detail/redis/RedisUtils.h"

#include <ranges>
#include <stdexcept>

namespace ruvia {
namespace detail {

RedisRegistry::RedisRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const RedisDefinition> redis)
    : resource_(detail::pmrResourceOrDefault(resource)),
      pools_(resource_) {
    pools_.reserve(redis.size());
    for (const auto& definition : redis) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("redis alias must not be empty");
        }
        if (std::ranges::any_of(
                pools_,
                [&definition](const Entry& entry) {
                    return std::string_view(entry.alias.data(), entry.alias.size()) ==
                        std::string_view(definition.alias);
                })) {
            throw std::invalid_argument("duplicate redis alias");
        }
        auto pool = makePmrObject<RedisPool>(resource_, ioContext, definition.config, resource_);
        pools_.push_back(Entry{
            std::pmr::string(definition.alias, resource_),
            std::move(pool)});
        if (std::string_view(pools_.back().alias.data(), pools_.back().alias.size()) == kDefaultRedisAlias) {
            defaultPool_ = pools_.back().pool.get();
        }
    }
}

RedisRegistry::~RedisRegistry() = default;

Task<void> RedisRegistry::connect() {
    for (auto& entry : pools_) {
        co_await entry.pool->connect();
    }
    co_return;
}

void RedisRegistry::closeNow() noexcept {
    for (auto& entry : pools_) {
        entry.pool->closeNow();
    }
}

bool RedisRegistry::empty() const noexcept {
    return pools_.empty();
}

bool RedisRegistry::hasAnyTimeout() const noexcept {
    return std::ranges::any_of(pools_, [](const Entry& entry) {
        return entry.pool->hasAnyTimeout();
    });
}

RedisHandle RedisRegistry::get(std::pmr::memory_resource* resource) const {
    if (defaultPool_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "default redis is not configured");
    }
    return RedisHandle(*defaultPool_, resource);
}

RedisHandle RedisRegistry::get(
    std::string_view alias,
    std::pmr::memory_resource* resource) const {
    for (const auto& entry : pools_) {
        if (std::string_view(entry.alias.data(), entry.alias.size()) == alias) {
            return RedisHandle(*entry.pool, resource);
        }
    }
    throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
}

void RedisRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : pools_) {
        entry.pool->scanDeadlines(now);
    }
}

}  // namespace detail

RedisHandle Context::redis() const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(resource());
}

RedisHandle Context::redis(std::string_view alias) const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(alias, resource());
}

}  // namespace ruvia
