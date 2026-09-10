#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbBackend.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

[[nodiscard]] std::pmr::string encodeDbCacheRows(const DbRows& rows, std::pmr::memory_resource* resource);
[[nodiscard]] DbRows decodeDbCacheRows(std::string_view bytes, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string dbCacheKey(std::string_view nameSpace, std::string_view id,
    std::string_view sql, std::span<const DbValue> params, DbDriver driver,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string dbCachePrefix(std::string_view nameSpace, std::pmr::memory_resource* resource);

class DbCacheQuery final {
public:
    DbCacheQuery(DbPoolRef pool, std::optional<std::size_t> slot,
        std::pmr::string sql, std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource, bool* backendFailed = nullptr) noexcept
        : pool_(pool),
          slot_(slot),
          sql_(std::move(sql)),
          params_(std::move(params)),
          resource_(pmrResourceOrDefault(resource)),
          backendFailed_(backendFailed) {}

    DbCacheQuery(const DbCacheQuery&) = delete;
    DbCacheQuery& operator=(const DbCacheQuery&) = delete;
    DbCacheQuery(DbCacheQuery&&) noexcept = default;
    DbCacheQuery& operator=(DbCacheQuery&&) = delete;

    // This is deliberately not a coroutine. Moving the owned statement into
    // executeOwned() makes the options local to that coroutine frame before a
    // lazy transaction backend task borrows them.
    [[nodiscard]] Task<DbRows> operator()(OperationOptions options) &&;

private:
    static Task<DbRows> executeOwned(DbPoolRef pool, std::optional<std::size_t> slot,
        std::pmr::string sql, std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource, bool* backendFailed,
        OperationOptions options);

    DbPoolRef pool_;
    std::optional<std::size_t> slot_;
    std::pmr::string sql_;
    std::pmr::vector<DbValue> params_;
    std::pmr::memory_resource* resource_;
    bool* backendFailed_;
};

[[nodiscard]] inline OperationOptions dbCacheRemainingOptions(
    const OperationOptions& base, const OperationTimeout& timeout) {
    auto result = base;
    result.timeout = timeout.remaining();
    return result;
}

[[noreturn]] inline void throwDbCacheTimeout() {
    throw DbError(DbError::Code::kTimeout, std::nullopt,
        "database query cache operation timed out");
}

[[nodiscard]] inline OperationOptions dbCacheRequiredOptions(
    const OperationOptions& base, const OperationTimeout& timeout) {
    auto result = dbCacheRemainingOptions(base, timeout);
    if (result.timeout.has_value() && result.timeout->count() == 0) {
        throwDbCacheTimeout();
    }
    return result;
}

// Store, key, and deferred database query belong to the cold frame. On a hit
// the query object is destroyed without starting. Decoded results own their
// fields independently of both Redis replies and subsequent cache operations.
template <typename Store, typename Database>
Task<DbRows> queryDbCache(Store store, std::pmr::string key, std::chrono::milliseconds duration,
    bool ignoreErrors, Database database, std::pmr::memory_resource* resource,
    OperationOptions options, std::optional<OperationTimeout> deadline = std::nullopt) {
    const auto started = std::chrono::steady_clock::now();
    const auto operationTimeout = deadline.has_value()
                                      ? *deadline
                                      : OperationTimeout(options.timeout);
    try {
        auto cached = co_await store.get(key, dbCacheRequiredOptions(options, operationTimeout));
        if (cached) {
            auto rows = decodeDbCacheRows(*cached, resource);
            if (operationTimeout.expired()) {
                throwDbCacheTimeout();
            }
            co_return rows;
        }
    } catch (const RedisError& error) {
        if (error.code() == RedisError::Code::kCancelled ||
            error.code() == RedisError::Code::kClosing) {
            throw;
        }
        if (operationTimeout.expired()) {
            throwDbCacheTimeout();
        }
        if (!ignoreErrors) {
            throw;
        }
    } catch (const DbConversionError&) {
        if (operationTimeout.expired()) {
            throwDbCacheTimeout();
        }
        if (!ignoreErrors) {
            throw;
        }
    }
    if (operationTimeout.expired()) {
        throwDbCacheTimeout();
    }
    auto rows = co_await std::move(database)(dbCacheRemainingOptions(options, operationTimeout));
    if (operationTimeout.expired()) {
        throwDbCacheTimeout();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed < duration) {
        auto bytes = encodeDbCacheRows(rows, resource);
        try {
            co_await store.put(key, bytes, duration - elapsed,
                dbCacheRequiredOptions(options, operationTimeout));
        } catch (const RedisError& error) {
            if (error.code() == RedisError::Code::kCancelled ||
                error.code() == RedisError::Code::kClosing) {
                throw;
            }
            if (operationTimeout.expired()) {
                throwDbCacheTimeout();
            }
            if (!ignoreErrors) {
                throw;
            }
        }
        if (operationTimeout.expired()) {
            throwDbCacheTimeout();
        }
    }
    if (operationTimeout.expired()) {
        throwDbCacheTimeout();
    }
    co_return rows;
}

}  // namespace ruvia::detail
