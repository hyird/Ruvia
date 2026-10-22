#pragma once

#include <chrono>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_config_storage.h"
#include "db/db_query_cache.h"

namespace ruvia::detail {

// One database alias's Redis-backed query-result cache (Redis builds only).
class db_query_cache_state final {
public:
    db_query_cache_state(const redis_handle& redis, const db_cache_config_storage& config, std::string_view identity,
        std::pmr::memory_resource* resource);
    ~db_query_cache_state();
    void close_now() noexcept;
    [[nodiscard]] std::optional<std::pmr::string> key(
        const db_query& query, const db_statement& statement, db_driver driver);
    // Serves the step from the cache, or runs `database` and stores its rows.
    [[nodiscard]] task<db_rows> wrap(std::optional<std::chrono::milliseconds> duration, std::pmr::string key,
        db_cache_query database, operation_options options, operation_timeout deadline);
    [[nodiscard]] task<void> remove(
        std::span<const std::string_view> ids, operation_scope& scope, operation_options options);
    [[nodiscard]] task<void> clear(operation_scope& scope, operation_options options);

private:
    void require_open() const;
    task<db_rows> query(std::pmr::string key, std::chrono::milliseconds duration, db_cache_query database,
        operation_options options, operation_timeout deadline);

    bool closed_{false};
    std::pmr::memory_resource* resource_;
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::pmr::string name_space_;
    redis_handle redis_;
};

}  // namespace ruvia::detail
