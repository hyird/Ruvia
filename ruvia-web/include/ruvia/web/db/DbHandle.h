#pragma once

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/detail/db/DbBackend.h"
#include "ruvia/web/detail/db/DbMappedQuery.h"
#include "ruvia/web/detail/db/DbParameterPack.h"

namespace ruvia {

class DbQuery;
template <typename Entity>
class DbEntityRows;
template <typename Entity, typename Executor>
class DbRepository;
template <typename Entity, typename Executor>
class DbQueryBuilder;

class DbHandle final : private detail::ScopedCapabilityNode {
public:
    DbHandle(const DbHandle& other) noexcept;
    DbHandle& operator=(const DbHandle&) = delete;

    [[nodiscard]] DbHandle withOptions(OperationOptions options) const;

    template <typename Entity>
    [[nodiscard]] DbRepository<Entity, DbHandle> getRepository() const;

    [[nodiscard]] ScopedOperation<DbRows> query(const DbQuery& query) const;
    [[nodiscard]] ScopedOperation<DbExecResult> execute(const DbQuery& query) const;
    template <typename Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> query(const DbQuery& query) const;

    ScopedOperation<DbRows> query(std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<DbRows> query(
        std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    ScopedOperation<DbExecResult> execute(
        std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<DbExecResult> execute(
        std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    ScopedOperation<DbStreamResult> queryStream(
        std::string_view sql, std::span<const DbValue> params = {}) const;
    ScopedOperation<DbStreamResult> queryStream(
        std::string_view sql, std::initializer_list<DbValue> params) const = delete;

    // Bound parameters as ordinary arguments: query(sql, id, name). Each one is
    // converted to a DbValue and cloned into the prepared statement before the
    // call returns -- the span overloads below build the statement synchronously,
    // never inside the returned operation -- so argument temporaries only have to
    // outlive the call itself, not the operation that is awaited afterwards.
    //
    // A caller that already holds a contiguous parameter sequence passes it as a
    // span instead; a span cannot construct a DbValue, so it never reaches here.
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view sql, Params&&... params) const {
        const DbValue values[]{detail::makeImmediateDbParameter(std::forward<Params>(params))...};
        return query(sql, std::span<const DbValue>(values));
    }

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbExecResult> execute(
        std::string_view sql, Params&&... params) const {
        const DbValue values[]{detail::makeImmediateDbParameter(std::forward<Params>(params))...};
        return execute(sql, std::span<const DbValue>(values));
    }

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbStreamResult> queryStream(
        std::string_view sql, Params&&... params) const {
        const DbValue values[]{detail::makeImmediateDbParameter(std::forward<Params>(params))...};
        return queryStream(sql, std::span<const DbValue>(values));
    }

    ScopedOperation<DbTransaction> beginTransaction(
        DbTransactionOptions options = {}) const;

private:
    friend class detail::DbRegistry;
    template <typename, typename>
    friend class DbRepository;
    template <typename, typename>
    friend class DbQueryBuilder;

    [[nodiscard]] DbDriver queryDriver() const;
    [[nodiscard]] std::pmr::memory_resource* queryResource() const;
    [[nodiscard]] Task<DbRows> queryTask(const DbQuery& query) const;
    template <typename Result, typename Mapper>
    [[nodiscard]] ScopedOperation<Result> queryMapped(const DbQuery& query, Mapper mapper) const {
        requireActive();
        auto task = queryTask(query);
        return detail::makeScopedOperation(operationScope(),
            detail::mapDbQuery<Result>(std::move(task), resource_, std::move(mapper)));
    }

    DbHandle(detail::DbPoolRef client, std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& operationScope) noexcept;
    static Task<DbStreamResult> queryStreamPrepared(detail::DbPoolRef client, std::pmr::string sql,
        std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource,
        detail::ScopedOperationScope& operationScope, OperationOptions options);
    static Task<DbTransaction> beginTransactionPrepared(detail::DbPoolRef client,
        std::pmr::memory_resource* resource, detail::ScopedOperationScope& operationScope,
        OperationOptions operationOptions, DbTransactionOptions transactionOptions);

    detail::DbPoolRef client_;
    std::pmr::memory_resource* resource_;
    OperationOptions options_;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
};

}  // namespace ruvia
