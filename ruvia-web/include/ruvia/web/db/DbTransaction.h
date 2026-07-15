#pragma once

#include "ruvia/web/db/DbQueryResult.h"

#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ruvia {

class DbTransaction final {
public:
    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&& other) noexcept;
    DbTransaction& operator=(DbTransaction&&) = delete;
    ~DbTransaction();

    [[nodiscard]] bool active() const noexcept;
    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params = {});
    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params) = delete;
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params = {});
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params) = delete;
    Task<void> commit();
    Task<void> rollback();

private:
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;

    struct Lease final {
        Lease(
            detail::DbPoolRef client,
            std::size_t slot,
            std::pmr::memory_resource* resource) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        std::pmr::memory_resource* resource;
    };

    DbTransaction(
        detail::DbPoolRef client,
        std::size_t slot,
        std::pmr::memory_resource* resource) noexcept;
    Task<QueryResult> executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params);
    void reset() noexcept;

    class OperationGuard final {
    public:
        explicit OperationGuard(DbTransaction& owner);
        OperationGuard(const OperationGuard&) = delete;
        OperationGuard& operator=(const OperationGuard&) = delete;
        ~OperationGuard();

        [[nodiscard]] Lease& lease() noexcept { return *lease_; }
        void finishActive() noexcept;
        void finishClosed() noexcept;
        void finishFailed() noexcept;

    private:
        DbTransaction* owner_;
        Lease* lease_;
    };

    detail::DbOperationState<Lease> state_{};
};

}  // namespace ruvia
