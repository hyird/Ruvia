#pragma once

#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/detail/db/DbBackend.h"

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

class DbHandle final : private detail::ScopedCapabilityNode {
public:
    DbHandle(const DbHandle& other) noexcept;
    DbHandle& operator=(const DbHandle&) = delete;

    ScopedOperation<QueryResult> query(std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    ScopedOperation<QueryResult> execute(std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    ScopedOperation<DbStreamResult> queryStream(std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<DbStreamResult> queryStream(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    ScopedOperation<DbTransaction> beginTransaction() const;

private:
    friend class detail::DbRegistry;

    DbHandle(
        detail::DbPoolRef client,
        std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& operationScope) noexcept;
    static Task<QueryResult> executePrepared(
        detail::DbPoolRef client,
        std::pmr::string sql,
        std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource);
    static Task<DbStreamResult> queryStreamPrepared(
        detail::DbPoolRef client,
        std::pmr::string sql,
        std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& operationScope);
    static Task<DbTransaction> beginTransactionPrepared(
        detail::DbPoolRef client,
        std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& operationScope);

    detail::DbPoolRef client_;
    std::pmr::memory_resource* resource_;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
};

}  // namespace ruvia
