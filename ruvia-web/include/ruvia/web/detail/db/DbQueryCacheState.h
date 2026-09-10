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
#include "ruvia/web/detail/db/DbConfigStorage.h"
#include "ruvia/web/detail/db/DbQueryCache.h"

namespace asio {
class io_context;
}
namespace ruvia::detail {
class RedisRegistry;
class DbQueryCacheState final {
public:
    DbQueryCacheState(asio::io_context& io, const WorkerHandle& worker,
        const DbCacheConfigStorage& config, std::pmr::memory_resource* resource);
    ~DbQueryCacheState();
    Task<void> connect();
    void closeNow() noexcept;
    std::optional<std::pmr::string> key(const DbQuery& query, const DbStatement& statement, DbDriver driver);
    Task<DbRows> wrap(std::optional<std::chrono::milliseconds> duration, std::optional<std::pmr::string> key,
        DbCacheQuery database, ScopedOperationScope& scope, OperationOptions options,
        std::optional<OperationTimeout> deadline = std::nullopt);
    Task<void> remove(std::span<const std::string_view> ids, ScopedOperationScope& scope, OperationOptions options);
    Task<void> clear(ScopedOperationScope& scope, OperationOptions options);

private:
    std::pmr::memory_resource* resource_;
    std::chrono::milliseconds duration_;
    bool alwaysEnabled_;
    bool ignoreErrors_;
    std::pmr::string nameSpace_;
#ifdef RUVIA_ENABLE_REDIS
    std::unique_ptr<RedisRegistry, PmrObjectDeleter<RedisRegistry>> redis_;
#endif
};
}  // namespace ruvia::detail
