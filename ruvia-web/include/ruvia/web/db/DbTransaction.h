#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/db/DbRows.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbMappedQuery.h"
#include "ruvia/web/detail/db/DbParameterPack.h"

namespace ruvia {

class DbTransaction;
class DbQuery;
class DbStatement;
template <typename Entity>
class DbEntityRows;
template <typename Entity, typename Executor>
class DbRepository;
template <typename Entity, typename Executor>
class DbQueryBuilder;

namespace detail {
struct DbTransactionStartPlan;
template <typename Pool>
Task<DbTransaction> beginDbTransaction(Pool&, std::pmr::memory_resource*,
    OperationOptions, DbTransactionStartPlan);
}  // namespace detail

class DbTransaction final : private detail::ScopedCapabilityNode {
public:
    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    DbTransaction(DbTransaction&& other) noexcept;
    DbTransaction& operator=(DbTransaction&&) = delete;
    ~DbTransaction();

    [[nodiscard]] bool active() const noexcept;

    template <typename Entity>
    [[nodiscard]] DbRepository<Entity, DbTransaction> getRepository() &;
    template <typename Entity>
    DbRepository<Entity, DbTransaction> getRepository() && = delete;

    [[nodiscard]] ScopedOperation<DbRows> query(const DbQuery& query) &;
    ScopedOperation<DbRows> query(const DbQuery&) && = delete;
    [[nodiscard]] ScopedOperation<DbExecResult> execute(const DbQuery& query) &;
    ScopedOperation<DbExecResult> execute(const DbQuery&) && = delete;
    template <typename Entity>
    [[nodiscard]] ScopedOperation<DbEntityRows<Entity>> query(const DbQuery& query) &;
    template <typename Entity>
    ScopedOperation<DbEntityRows<Entity>> query(const DbQuery&) && = delete;
    ScopedOperation<DbRows> query(std::string_view sql, std::span<const DbValue> params = {}) &;
    ScopedOperation<DbRows> query(std::string_view, std::span<const DbValue> = {}) && = delete;
    ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) & = delete;
    ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) && = delete;
    ScopedOperation<DbExecResult> execute(
        std::string_view sql, std::span<const DbValue> params = {}) &;
    ScopedOperation<DbExecResult> execute(
        std::string_view, std::span<const DbValue> = {}) && = delete;
    ScopedOperation<DbExecResult> execute(
        std::string_view, std::initializer_list<DbValue>) & = delete;
    ScopedOperation<DbExecResult> execute(
        std::string_view, std::initializer_list<DbValue>) && = delete;

    // Bound parameters as ordinary arguments, with the same synchronous cloning
    // and the same temporary-safety as DbHandle::query()/execute().
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbRows> query(std::string_view sql, Params&&... params) & {
        const DbValue values[]{detail::makeImmediateDbParameter(std::forward<Params>(params))...};
        return query(sql, std::span<const DbValue>(values));
    }
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    ScopedOperation<DbRows> query(std::string_view, Params&&...) && = delete;

    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    [[nodiscard]] ScopedOperation<DbExecResult> execute(
        std::string_view sql, Params&&... params) & {
        const DbValue values[]{detail::makeImmediateDbParameter(std::forward<Params>(params))...};
        return execute(sql, std::span<const DbValue>(values));
    }
    template <typename... Params>
        requires detail::DbParameterPack<Params...>
    ScopedOperation<DbExecResult> execute(std::string_view, Params&&...) && = delete;

    ScopedOperation<void> commit() &;
    ScopedOperation<void> commit() && = delete;
    ScopedOperation<void> rollback() &;
    ScopedOperation<void> rollback() && = delete;

private:
    friend class DbHandle;
    template <typename, typename>
    friend class DbRepository;
    template <typename, typename>
    friend class DbQueryBuilder;

    [[nodiscard]] DbDriver queryDriver() const;
    [[nodiscard]] std::pmr::memory_resource* queryResource() const;
    [[nodiscard]] Task<DbRows> queryTask(const DbQuery& query);
    [[nodiscard]] Task<std::pair<DbRows, DbRows>> queryAndCountTask(const DbQuery& query, const DbQuery& count);
    template <typename Result, typename Mapper>
    [[nodiscard]] ScopedOperation<std::pair<Result, std::uint64_t>> queryMappedAndCount(const DbQuery& query, const DbQuery& count, Mapper mapper) {
        requireActive();
        auto* resource = queryResource();
        auto task = queryAndCountTask(query, count);
        return detail::makeScopedOperation(operationScope(),
            detail::mapDbQueryAndCount<Result>(std::move(task), resource, std::move(mapper)));
    }
    template <typename Result, typename Mapper>
    [[nodiscard]] ScopedOperation<Result> queryMapped(const DbQuery& query, Mapper mapper) {
        requireActive();
        auto* resource = queryResource();
        auto task = queryTask(query);
        return detail::makeScopedOperation(operationScope(),
            detail::mapDbQuery<Result>(std::move(task), resource, std::move(mapper)));
    }
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;
    template <typename Pool>
    friend Task<DbTransaction> detail::beginDbTransaction(
        Pool&, std::pmr::memory_resource*, OperationOptions,
        detail::DbTransactionStartPlan);

    struct Lease final {
        Lease(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource,
            OperationOptions options) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        std::pmr::memory_resource* resource;
        OperationOptions options;
    };

    using OperationState = detail::DbOperationState<Lease>;
    using OperationGuard = detail::DbOperationGuard<Lease>;
    static Task<std::pair<DbRows, DbRows>> queryAndCountPrepared(DbStatement query, DbStatement count, OperationGuard operation);

    class State;
    using StateOwner = std::unique_ptr<State, detail::PmrObjectDeleter<State>>;

    DbTransaction(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource,
        OperationOptions options);
    static Task<DbRows> queryPrepared(
        std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation);
    static Task<DbExecResult> executePrepared(
        std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation);
    static Task<void> commitTask(OperationGuard operation);
    static Task<void> rollbackTask(OperationGuard operation);
    void reset() noexcept;
    void bindOperationScope(detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    StateOwner state_;
};

}  // namespace ruvia
