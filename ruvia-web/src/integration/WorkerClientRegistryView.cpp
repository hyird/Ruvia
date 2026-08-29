#include "ruvia/web/detail/integration/WorkerClientRegistryView.h"

#include <optional>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia::detail {
namespace {

[[nodiscard]] OperationOptions contextOperationOptions(const StopToken& stopToken) {
    return OperationOptions{.timeout = std::nullopt, .stopToken = stopToken};
}

}  // namespace

#ifdef RUVIA_ENABLE_DATABASE
DbHandle WorkerClientRegistryView::db(std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope, const StopToken& stopToken) const {
    if (!attached()) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
    }
    return databases_->get(resource, operationScope)
        .withOptions(contextOperationOptions(stopToken));
}

DbHandle WorkerClientRegistryView::db(std::string_view alias, std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope, const StopToken& stopToken) const {
    if (!attached()) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
    }
    return databases_->get(alias, resource, operationScope)
        .withOptions(contextOperationOptions(stopToken));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle WorkerClientRegistryView::redis(std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope, const StopToken& stopToken) const {
    if (!attached()) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(resource, operationScope).withOptions(contextOperationOptions(stopToken));
}

RedisHandle WorkerClientRegistryView::redis(std::string_view alias,
    std::pmr::memory_resource* resource, ScopedOperationScope& operationScope,
    const StopToken& stopToken) const {
    if (!attached()) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(alias, resource, operationScope)
        .withOptions(contextOperationOptions(stopToken));
}
#endif

HttpClientHandle WorkerClientRegistryView::httpClient(std::pmr::memory_resource* resource,
    ScopedOperationScope& operationScope, const StopToken& stopToken) const {
    if (!attached()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "http client is not configured");
    }
    return httpClients_->get(resource, operationScope)
        .withOptions(contextOperationOptions(stopToken));
}

HttpClientHandle WorkerClientRegistryView::httpClient(std::string_view alias,
    std::pmr::memory_resource* resource, ScopedOperationScope& operationScope,
    const StopToken& stopToken) const {
    if (!attached()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "http client is not configured");
    }
    return httpClients_->get(alias, resource, operationScope)
        .withOptions(contextOperationOptions(stopToken));
}

}  // namespace ruvia::detail
