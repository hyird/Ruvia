#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_config_storage.h"
#include "db/db_query_cache.h"
namespace ruvia::detail {
class db_query_cache_state final {
public:
    db_query_cache_state(const redis_handle& redis,
        const db_cache_config_storage& config, std::string_view identity, std::pmr::memory_resource* resource);
    ~db_query_cache_state();
    void close_now() noexcept;
    std::optional<std::pmr::string> key(const db_query& query, const db_statement& statement, db_driver driver);
    task<db_rows> wrap(std::optional<std::chrono::milliseconds> duration, std::optional<std::pmr::string> key,
        db_cache_query database, operation_options options,
        std::optional<ruvia::operation_timeout> deadline = std::nullopt);
    task<void> remove(std::span<const std::string_view> ids, operation_options options);
    task<void> clear(operation_options options);

private:
    void require_open() const;
    bool closed_{false};
    std::pmr::memory_resource* resource_;
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::pmr::string name_space_;
#ifdef RUVIA_ENABLE_REDIS
    redis_handle redis_;
#endif
};
}  // namespace ruvia::detail
