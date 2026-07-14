#pragma once

#include "ruvia/web/db/Db.h"

#ifndef RUVIA_ENABLE_MARIADB

#include <asio/io_context.hpp>

#include <memory_resource>
#include <span>

namespace ruvia::detail {

class DbRegistry final {
public:
    DbRegistry(
        asio::io_context&,
        std::pmr::memory_resource*,
        std::span<const DbDefinition>) {}

    DbRegistry(const DbRegistry&) = delete;
    DbRegistry& operator=(const DbRegistry&) = delete;

    [[nodiscard]] Task<void> connect() {
        co_return;
    }

    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept { return true; }
    void scanDeadlines() noexcept {}
    [[nodiscard]] bool hasAnyTimeout() const noexcept { return false; }
};

}  // namespace ruvia::detail

#else

#include <asio/io_context.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/detail/PoolWaiterQueue.h"

struct st_mysql;
struct st_mysql_res;

namespace ruvia::detail {

inline constexpr std::string_view kDefaultDbAlias = "default";

// Holds the per-connection ASIO socket used to await MariaDB non-blocking I/O.
// Defined in Db.cpp so the heavy ASIO/winsock headers stay out of this header.
struct SlotSocket;

class MariaDbPool final {
public:
    MariaDbPool(
        asio::io_context& ioContext,
        DbConfig config,
        std::pmr::memory_resource* resource = nullptr);
    ~MariaDbPool();

    MariaDbPool(const MariaDbPool&) = delete;
    MariaDbPool& operator=(const MariaDbPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;

private:
    friend class ::ruvia::DbHandle;
    friend class ::ruvia::DbTransaction;
    friend class ::ruvia::DbStreamResult;

    class SlotGuard final {
    public:
        SlotGuard(MariaDbPool& client, std::size_t slot) noexcept;
        SlotGuard(const SlotGuard&) = delete;
        SlotGuard& operator=(const SlotGuard&) = delete;
        ~SlotGuard();

    private:
        MariaDbPool* client_;
        std::size_t slot_;
    };

    struct ConnectionSlot {
        // Defined out-of-line where SlotSocket is complete, so the
        // unique_ptr<SlotSocket> member can be destroyed without exposing the
        // ASIO/MariaDB socket adapter through this internal boundary.
        explicit ConnectionSlot(std::pmr::memory_resource* resource = nullptr) noexcept;
        ~ConnectionSlot();
        ConnectionSlot(ConnectionSlot&&) noexcept;
        ConnectionSlot& operator=(ConnectionSlot&&) noexcept;

        using SlotSocketDeleter = PmrObjectDeleter<SlotSocket>;

        st_mysql* connection{nullptr};
        std::unique_ptr<SlotSocket, SlotSocketDeleter> waitSocket;
        std::chrono::steady_clock::time_point deadline{};
        std::coroutine_handle<> deadlineContinuation{};
        bool busy{false};
        bool connected{false};
        bool deadlineActive{false};
        bool timedOut{false};
        enum class DeadlineKind : std::uint8_t {
            kNone,
            kSocket,
            kSleep
        };
        DeadlineKind deadlineKind{DeadlineKind::kNone};
    };

    Task<std::size_t> acquireSlot();
    void releaseSlot(std::size_t slot) noexcept;
    void closeSlot(ConnectionSlot& slot) noexcept;
    void setSlotDeadline(ConnectionSlot& slot, std::chrono::milliseconds timeout, ConnectionSlot::DeadlineKind kind) noexcept;
    void clearSlotDeadline(ConnectionSlot& slot) noexcept;
    Task<void> connectUnlocked(ConnectionSlot& slot);
    struct OperationDeadline;
    Task<int> waitForMysql(ConnectionSlot& slot, int status, const OperationDeadline& deadline);
    Task<void> runMysqlQuery(ConnectionSlot& slot, std::string_view sql, const OperationDeadline& deadline);
    Task<st_mysql_res*> storeMysqlResult(ConnectionSlot& slot, const OperationDeadline& deadline);
    Task<QueryResult> executeOnSlot(
        ConnectionSlot& slot,
        std::string_view sql,
        std::span<const DbValue> params,
        std::pmr::memory_resource* resource);
    Task<void> executeControl(ConnectionSlot& slot, std::string_view sql, std::pmr::memory_resource* resource);
    Task<QueryResult> execute(
        std::pmr::string sql,
        std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource);
    Task<DbStreamResult> stream(
        std::pmr::string sql,
        std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource);
    Task<std::optional<DbRow>> readStreamRow(
        std::size_t slot,
        void* result,
        std::pmr::memory_resource* resource);
    Task<void> closeStream(std::size_t slot, void* result, std::pmr::memory_resource* resource);
    void abortStream(std::size_t slot, void* result) noexcept;
    Task<QueryResult> executeOnTransactionSlot(
        std::size_t slot,
        std::pmr::string sql,
        std::pmr::vector<DbValue> params,
        std::pmr::memory_resource* resource);
    Task<DbTransaction> beginTransaction(std::pmr::memory_resource* resource, RequestMemory* requestMemory);
    Task<void> commitTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    Task<void> rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    void abortTransaction(std::size_t slot) noexcept;

    asio::io_context& ioContext_;
    DbConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<ConnectionSlot> slots_;
    std::pmr::vector<std::size_t> freeSlots_;
    PoolWaiterQueue waiters_;
    bool closing_{false};
};

class DbRegistry final {
public:
    DbRegistry(
        asio::io_context& ioContext,
        std::pmr::memory_resource* resource,
        std::span<const DbDefinition> databases);
    ~DbRegistry();

    DbRegistry(const DbRegistry&) = delete;
    DbRegistry& operator=(const DbRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    [[nodiscard]] DbHandle get(std::pmr::memory_resource* resource, RequestMemory* requestMemory = nullptr) const;
    [[nodiscard]] DbHandle get(
        std::string_view alias,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) const;

private:
    using MariaDbPoolDeleter = PmrObjectDeleter<MariaDbPool>;

    struct Entry {
        std::pmr::string alias;
        std::unique_ptr<MariaDbPool, MariaDbPoolDeleter> client;
    };

    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> clients_;
    MariaDbPool* defaultClient_{nullptr};
};

}  // namespace ruvia::detail

#endif
