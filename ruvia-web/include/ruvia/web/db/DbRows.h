#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbBackend.h"
#include "ruvia/web/detail/db/DbOperationState.h"
#include "ruvia/core/memory/PmrResource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DbRow& operator[](std::size_t index) const& noexcept;
    [[nodiscard]] const DbRow& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const DbRow* begin() const& noexcept;
    [[nodiscard]] const DbRow* begin() const&& = delete;
    [[nodiscard]] const DbRow* end() const& noexcept;
    [[nodiscard]] const DbRow* end() const&& = delete;
    [[nodiscard]] const DbRow& front() const& noexcept;
    [[nodiscard]] const DbRow& front() const&& = delete;

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
    std::pmr::vector<std::pmr::string> columnNames_;
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

    constexpr DbExecResult(
        std::uint64_t affectedRows, std::optional<std::uint64_t> lastInsertId) noexcept
        : affectedRows_(affectedRows),
          lastInsertId_(lastInsertId) {}

    std::uint64_t affectedRows_{0};
    std::optional<std::uint64_t> lastInsertId_;
};

class DbStreamResult final : private detail::ScopedCapabilityNode {
public:
    DbStreamResult(const DbStreamResult&) = delete;
    DbStreamResult& operator=(const DbStreamResult&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    DbStreamResult(DbStreamResult&& other) noexcept;
    DbStreamResult& operator=(DbStreamResult&&) = delete;
    ~DbStreamResult();

    [[nodiscard]] bool active() const noexcept;
    ScopedOperation<std::optional<DbRow>> read() &;
    ScopedOperation<std::optional<DbRow>> read() && = delete;
    ScopedOperation<void> close() &;
    ScopedOperation<void> close() && = delete;

private:
    friend class DbHandle;
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;

    struct Lease final {
        Lease(detail::DbPoolRef client, std::size_t slot, void* result,
            std::pmr::memory_resource* resource, OperationOptions options) noexcept;

        detail::DbPoolRef client;
        std::size_t slot;
        void* result;
        std::pmr::memory_resource* resource;
        OperationOptions options;
    };

    using OperationState = detail::DbOperationState<Lease>;
    using OperationGuard = detail::DbOperationGuard<Lease>;

    class State;
    using StateOwner = std::unique_ptr<State, detail::PmrObjectDeleter<State>>;

    DbStreamResult() noexcept = default;
    DbStreamResult(detail::DbPoolRef client, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, OperationOptions options);
    void reset() noexcept;
    void bindOperationScope(detail::ScopedOperationScope& scope) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& capability) noexcept;
    static Task<std::optional<DbRow>> readTask(OperationGuard operation);
    static Task<void> closeTask(OperationGuard operation);

    StateOwner state_;
};

}  // namespace ruvia
