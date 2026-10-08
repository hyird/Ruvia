#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/db/DbQuery.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/redis/RedisHandle.h"

#include "db/DbConfigStorage.h"
#include "db/DbQueryCache.h"
namespace ruvia::detail {
class DbQueryCacheState final {
public:
    DbQueryCacheState(const RedisHandle& redis,
        const DbCacheConfigStorage& config, std::string_view identity, std::pmr::memory_resource* resource);
    ~DbQueryCacheState();
    void closeNow() noexcept;
    std::optional<std::pmr::string> key(const DbQuery& query, const DbStatement& statement, DbDriver driver);
    Task<DbRows> wrap(std::optional<std::chrono::milliseconds> duration, std::optional<std::pmr::string> key,
        DbCacheQuery database, OperationOptions options,
        std::optional<ruvia::OperationTimeout> deadline = std::nullopt);
    Task<void> remove(std::span<const std::string_view> ids, OperationOptions options);
    Task<void> clear(OperationOptions options);

private:
    void require_open() const;
    bool closed_{false};
    std::pmr::memory_resource* resource_;
    std::chrono::milliseconds duration_;
    bool alwaysEnabled_;
    bool ignoreErrors_;
    std::pmr::string nameSpace_;
#ifdef RUVIA_ENABLE_REDIS
    RedisHandle redis_;
#endif
};
}  // namespace ruvia::detail
