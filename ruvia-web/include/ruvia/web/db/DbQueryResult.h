#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/web/ScopedOperation.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbBackend.h"
#include "ruvia/web/detail/db/DbOperationState.h"
#include "ruvia/core/memory/PmrResource.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

struct st_mysql_res;

namespace ruvia {

class QueryResult final {
public:
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;
    QueryResult(QueryResult&& other) noexcept;
    QueryResult& operator=(QueryResult&&) = delete;
    ~QueryResult();

    [[nodiscard]] std::span<const DbRow> rows() const & noexcept;
    [[nodiscard]] std::span<const DbRow> rows() const && = delete;
    [[nodiscard]] std::uint64_t affectedRows() const noexcept;
    [[nodiscard]] std::uint64_t lastInsertId() const noexcept;

private:
    friend struct detail::DbResultAccess;

    struct NoRawResult final {};

    struct OwnedRawResult final {
        OwnedRawResult(
            void* ownedValue,
            void (*ownedRelease)(void*) noexcept) noexcept
            : value(ownedValue),
              release(ownedRelease) {}

        void* value;
        void (*release)(void*) noexcept;
    };

    explicit QueryResult(std::pmr::memory_resource* resource = nullptr);
    QueryResult(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    std::pmr::vector<DbRow> rows_;
    std::pmr::vector<DbField> fields_;
    std::uint64_t affectedRows_{0};
    std::uint64_t lastInsertId_{0};
    std::variant<NoRawResult, OwnedRawResult> rawResult_;
};

class DbStreamResult final : private detail::ScopedCapabilityNode {
public:
    DbStreamResult(const DbStreamResult&) = delete;
    DbStreamResult& operator=(const DbStreamResult&) = delete;
    DbStreamResult(DbStreamResult&& other) noexcept;
    DbStreamResult& operator=(DbStreamResult&&) = delete;
    ~DbStreamResult();

    [[nodiscard]] bool active() const noexcept;
    ScopedOperation<std::optional<DbRow>> read();
    ScopedOperation<void> close();

private:
    friend class DbHandle;
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;

    struct Lease final {
        Lease(
            detail::DbPoolRef client,
            std::size_t slot,
            void* result,
            std::pmr::memory_resource* resource) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        void* result;
        std::pmr::memory_resource* resource;
    };

    DbStreamResult() noexcept = default;
    DbStreamResult(
        detail::DbPoolRef client,
        std::size_t slot,
        void* result,
        std::pmr::memory_resource* resource) noexcept;
    void reset() noexcept;
    void bindOperationScope(detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
    Task<std::optional<DbRow>> readTask();
    Task<void> closeTask();

    template <typename Owner>
    friend class detail::DbOperationGuard;
    using OperationGuard = detail::DbOperationGuard<DbStreamResult>;

    detail::DbOperationState<Lease> state_{};
    detail::ScopedOperationScope operationScope_;
};

}  // namespace ruvia
