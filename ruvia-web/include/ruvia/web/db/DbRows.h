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

class DbRows final {
public:
    DbRows(const DbRows&) = delete;
    DbRows& operator=(const DbRows&) = delete;
    DbRows(DbRows&& other) noexcept;
    DbRows& operator=(DbRows&&) = delete;
    ~DbRows();

    [[nodiscard]] std::span<const DbRow> rows() const& noexcept;
    [[nodiscard]] std::span<const DbRow> rows() const&& = delete;

private:
    friend struct detail::DbResultAccess;

    struct NoRawResult final {};

    struct OwnedRawResult final {
        OwnedRawResult(void* ownedValue, void (*ownedRelease)(void*) noexcept) noexcept
            : value(ownedValue),
              release(ownedRelease) {}

        void* value;
        void (*release)(void*) noexcept;
    };

    explicit DbRows(std::pmr::memory_resource* resource = nullptr);
    DbRows(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    std::pmr::vector<DbRow> rows_;
    std::pmr::vector<DbField> fields_;
    std::uint64_t affectedRows_{0};
    std::optional<std::uint64_t> lastInsertId_;
    std::variant<NoRawResult, OwnedRawResult> rawResult_;
};

// Result of a statement whose contract is side effects rather than a row set.
// PostgreSQL does not expose a portable connection-level insert id, so that
// value is present only when the selected backend supplied one.
class DbExecResult final {
public:
    [[nodiscard]] constexpr std::uint64_t affectedRows() const noexcept {
        return affectedRows_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> lastInsertId() const noexcept {
        return lastInsertId_;
    }

private:
    friend struct detail::DbResultAccess;

    constexpr DbExecResult(std::uint64_t affectedRows, std::optional<std::uint64_t> lastInsertId) noexcept
        : affectedRows_(affectedRows), lastInsertId_(lastInsertId) {}

    std::uint64_t affectedRows_{0};
    std::optional<std::uint64_t> lastInsertId_;
};

class DbStreamResult final : private detail::ScopedCapabilityNode {
public:
    DbStreamResult(const DbStreamResult&) = delete;
    DbStreamResult& operator=(const DbStreamResult&) = delete;
    // A pending read/close task captures this object. Moving it while that
    // task is cold would leave the frame pointing at the moved-from object.
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
        Lease(detail::DbPoolRef client, std::size_t slot, void* result, std::pmr::memory_resource* resource) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        void* result;
        std::pmr::memory_resource* resource;
    };

    DbStreamResult() noexcept = default;
    DbStreamResult(detail::DbPoolRef client, std::size_t slot, void* result, std::pmr::memory_resource* resource) noexcept;
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
