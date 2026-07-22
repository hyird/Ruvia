#include "ruvia/web/Context.h"

#include <stdexcept>

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/detail/db/DbInternal.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/detail/redis/RedisInternal.h"
#endif

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
DbHandle Context::db() const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(resource(), operationScope_);
}

DbHandle Context::db(std::string_view alias) const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(alias, resource(), operationScope_);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle Context::redis() const {
    if (redis_ == nullptr) {
        throw RedisError(
            RedisError::Code::kNotConfigured,
            "redis is not configured");
    }
    return redis_->get(resource(), operationScope_);
}

RedisHandle Context::redis(std::string_view alias) const {
    if (redis_ == nullptr) {
        throw RedisError(
            RedisError::Code::kNotConfigured,
            "redis is not configured");
    }
    return redis_->get(alias, resource(), operationScope_);
}
#endif

}  // namespace ruvia
