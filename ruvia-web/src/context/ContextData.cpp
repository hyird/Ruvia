#include "ruvia/web/Context.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"
#include "ruvia/web/detail/db/DbRegistry.h"

#include <optional>
#include <stdexcept>

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
DbHandle Context::db() const {
    if (db_ == nullptr) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
    }
    return db_->get(resource(), operationScope_)
        .withOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}

DbHandle Context::db(std::string_view alias) const {
    if (db_ == nullptr) {
        throw DbError(DbError::Code::kNotConfigured, std::nullopt, "database is not configured");
    }
    return db_->get(alias, resource(), operationScope_)
        .withOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle Context::redis() const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(resource(), operationScope_)
        .withOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}

RedisHandle Context::redis(std::string_view alias) const {
    if (redis_ == nullptr) {
        throw RedisError(RedisError::Code::kNotConfigured, "redis is not configured");
    }
    return redis_->get(alias, resource(), operationScope_)
        .withOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}
#endif

}  // namespace ruvia
