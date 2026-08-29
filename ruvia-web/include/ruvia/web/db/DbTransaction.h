#pragma once

#include "ruvia/web/db/DbRows.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/detail/db/DbParameterPack.h"

#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ruvia {

class DbTransaction final : private detail::ScopedCapabilityNode {
public:
    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    // A pending query/commit/rollback task captures this object. Moving it
    // while that task is cold would leave the frame pointing at the
    // moved-from object.
    DbTransaction(DbTransaction&& other) noexcept;
    DbTransaction& operator=(DbTransaction&&) = delete;
    ~DbTransaction();

    [[nodiscard]] bool active() const noexcept;
    ScopedOperation<DbRows> query(std::string_view sql, std::span<const DbValue> params = {}) &;
    ScopedOperation<DbRows> query(std::string_view, std::span<const DbValue> = {}) && = delete;
    ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) & = delete;
    ScopedOperation<DbRows> query(std::string_view, std::initializer_list<DbValue>) && = delete;
    ScopedOperation<DbExecResult> execute(std::string_view sql, std::span<const DbValue> params = {}) &;
    ScopedOperation<DbExecResult> execute(std::string_view, std::span<const DbValue> = {}) && = delete;
    ScopedOperation<DbExecResult> execute(std::string_view, std::initializer_list<DbValue>) & = delete;
    ScopedOperation<DbExecResult> execute(std::string_view, std::initializer_list<DbValue>) && = delete;

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
    [[nodiscard]] ScopedOperation<DbExecResult> execute(std::string_view sql, Params&&... params) & {
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
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;

    template <typename Owner>
    friend class detail::DbOperationGuard;
    using OperationGuard = detail::DbOperationGuard<DbTransaction>;

    struct Lease final {
        Lease(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        std::pmr::memory_resource* resource;
        OperationOptions options;
    };

    DbTransaction(detail::DbPoolRef client, std::size_t slot, std::pmr::memory_resource* resource, OperationOptions options) noexcept;
    Task<DbRows> queryPrepared(std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation);
    Task<DbExecResult> executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params, OperationGuard operation);
    Task<void> commitTask(OperationGuard operation);
    Task<void> rollbackTask(OperationGuard operation);
    void reset() noexcept;
    void bindOperationScope(detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;

    detail::DbOperationState<Lease> state_{};
    detail::ScopedOperationScope operationScope_;
};

}  // namespace ruvia
