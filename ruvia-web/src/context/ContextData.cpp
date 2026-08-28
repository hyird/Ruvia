#include "ruvia/web/Context.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/DbHandle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/RedisHandle.h"
#endif

namespace ruvia {

#ifdef RUVIA_ENABLE_DATABASE
DbHandle Context::db() const {
    return clientRegistries_.db(resource(), operationScope_, stopToken_);
}

DbHandle Context::db(std::string_view alias) const {
    return clientRegistries_.db(alias, resource(), operationScope_, stopToken_);
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle Context::redis() const {
    return clientRegistries_.redis(resource(), operationScope_, stopToken_);
}

RedisHandle Context::redis(std::string_view alias) const {
    return clientRegistries_.redis(alias, resource(), operationScope_, stopToken_);
}
#endif

}  // namespace ruvia
