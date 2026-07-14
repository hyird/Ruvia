#pragma once

#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbBackend.h"

#if !defined(RUVIA_ENABLE_MARIADB) && !defined(RUVIA_ENABLE_POSTGRESQL)

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

    [[nodiscard]] Task<void> connect() { co_return; }
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
#include <variant>
#include <vector>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/db/DbPoolDeadline.h"
#include "ruvia/web/detail/db/DbPoolScheduler.h"

struct st_mysql;
struct st_mysql_res;
struct pg_conn;
struct pg_result;

namespace ruvia::detail {

inline constexpr std::string_view kDefaultDbAlias = "default";

struct DbSlotSocket;

#ifdef RUVIA_ENABLE_MARIADB

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

public:
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
        explicit ConnectionSlot(std::pmr::memory_resource* resource = nullptr) noexcept;
        ~ConnectionSlot();
        ConnectionSlot(ConnectionSlot&&) noexcept;
        ConnectionSlot& operator=(ConnectionSlot&&) noexcept;

        using SlotSocketDeleter = PmrObjectDeleter<DbSlotSocket>;

        st_mysql* connection{nullptr};
        std::unique_ptr<DbSlotSocket, SlotSocketDeleter> waitSocket;
        std::chrono::steady_clock::time_point deadline{};
        std::coroutine_handle<> deadlineContinuation{};
        bool connected{false};
        bool deadlineActive{false};
        bool timedOut{false};
        enum class DeadlineKind : std::uint8_t { kNone, kSocket, kSleep };
        DeadlineKind deadlineKind{DeadlineKind::kNone};
    };

public:
    // Backend dispatch entry points. The class itself remains detail-only.
    Task<std::size_t> acquireSlot();
    void releaseSlot(std::size_t slot) noexcept;
    void closeSlot(ConnectionSlot& slot) noexcept;
    void setSlotDeadline(ConnectionSlot& slot, std::chrono::milliseconds timeout, ConnectionSlot::DeadlineKind kind) noexcept;
    void clearSlotDeadline(ConnectionSlot& slot) noexcept;
    Task<void> connectUnlocked(ConnectionSlot& slot);
    Task<int> waitForMysql(ConnectionSlot& slot, int status, const DbOperationDeadline& deadline);
    Task<void> runMysqlQuery(ConnectionSlot& slot, std::string_view sql, const DbOperationDeadline& deadline);
    Task<st_mysql_res*> storeMysqlResult(ConnectionSlot& slot, const DbOperationDeadline& deadline);
    Task<QueryResult> executeOnSlot(ConnectionSlot& slot, std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource);
    Task<void> executeControl(ConnectionSlot& slot, std::string_view sql, std::pmr::memory_resource* resource);
    Task<QueryResult> execute(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<DbStreamResult> stream(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<std::optional<DbRow>> readStreamRow(std::size_t slot, void* result, std::pmr::memory_resource* resource);
    Task<void> closeStream(std::size_t slot, void* result, std::pmr::memory_resource* resource);
    void abortStream(std::size_t slot, void* result) noexcept;
    Task<QueryResult> executeOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<DbTransaction> beginTransaction(std::pmr::memory_resource* resource);
    Task<void> commitTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    Task<void> rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    void abortTransaction(std::size_t slot) noexcept;

private:
    asio::io_context& ioContext_;
    DbConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<ConnectionSlot> slots_;
    DbPoolScheduler scheduler_;
};

#endif  // RUVIA_ENABLE_MARIADB

#ifdef RUVIA_ENABLE_POSTGRESQL

class PostgreSqlPool final {
public:
    PostgreSqlPool(
        asio::io_context& ioContext,
        DbConfig config,
        std::pmr::memory_resource* resource = nullptr);
    ~PostgreSqlPool();

    PostgreSqlPool(const PostgreSqlPool&) = delete;
    PostgreSqlPool& operator=(const PostgreSqlPool&) = delete;

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
        SlotGuard(PostgreSqlPool& client, std::size_t slot) noexcept;
        SlotGuard(const SlotGuard&) = delete;
        SlotGuard& operator=(const SlotGuard&) = delete;
        ~SlotGuard();

    private:
        PostgreSqlPool* client_;
        std::size_t slot_;
    };

    struct ConnectionSlot {
        explicit ConnectionSlot(std::pmr::memory_resource* resource = nullptr) noexcept;
        ~ConnectionSlot();
        ConnectionSlot(ConnectionSlot&&) noexcept;
        ConnectionSlot& operator=(ConnectionSlot&&) noexcept;

        using SlotSocketDeleter = PmrObjectDeleter<DbSlotSocket>;

        pg_conn* connection{nullptr};
        std::unique_ptr<DbSlotSocket, SlotSocketDeleter> waitSocket;
        std::chrono::steady_clock::time_point deadline{};
        bool connected{false};
        bool deadlineActive{false};
        bool timedOut{false};
    };

public:
    // Backend dispatch entry points. The class itself remains detail-only.
    Task<std::size_t> acquireSlot();
    void releaseSlot(std::size_t slot) noexcept;
    void closeSlot(ConnectionSlot& slot) noexcept;
    void setSlotDeadline(ConnectionSlot& slot, std::optional<std::chrono::milliseconds> timeout) noexcept;
    void clearSlotDeadline(ConnectionSlot& slot) noexcept;
    Task<void> connectUnlocked(ConnectionSlot& slot);
    Task<void> waitForPostgreSql(ConnectionSlot& slot, bool read, const DbOperationDeadline& deadline);
    Task<void> flushOutput(ConnectionSlot& slot, const DbOperationDeadline& deadline);
    Task<void> waitUntilResultReady(ConnectionSlot& slot, const DbOperationDeadline& deadline);
    Task<void> sendQuery(ConnectionSlot& slot, const std::pmr::string& sql, std::span<const DbValue> params, const DbOperationDeadline& deadline, bool singleRow);
    Task<QueryResult> executeOnSlot(ConnectionSlot& slot, const std::pmr::string& sql, std::span<const DbValue> params, std::pmr::memory_resource* resource);
    Task<void> executeControl(ConnectionSlot& slot, std::string_view sql, std::pmr::memory_resource* resource);
    Task<QueryResult> execute(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<DbStreamResult> stream(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<std::optional<DbRow>> readStreamRow(std::size_t slot, void* result, std::pmr::memory_resource* resource);
    Task<void> closeStream(std::size_t slot, void* result, std::pmr::memory_resource* resource);
    void abortStream(std::size_t slot, void* result) noexcept;
    Task<QueryResult> executeOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource);
    Task<DbTransaction> beginTransaction(std::pmr::memory_resource* resource);
    Task<void> commitTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    Task<void> rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource);
    void abortTransaction(std::size_t slot) noexcept;

private:
    asio::io_context& ioContext_;
    DbConfig config_;
    std::pmr::memory_resource* resource_;
    std::pmr::vector<ConnectionSlot> slots_;
    DbPoolScheduler scheduler_;
};

#endif  // RUVIA_ENABLE_POSTGRESQL

class DbRegistry final {
public:
    DbRegistry(asio::io_context& ioContext, std::pmr::memory_resource* resource, std::span<const DbDefinition> databases);
    ~DbRegistry();

    DbRegistry(const DbRegistry&) = delete;
    DbRegistry& operator=(const DbRegistry&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;
    [[nodiscard]] DbHandle get(std::pmr::memory_resource* resource) const;
    [[nodiscard]] DbHandle get(
        std::string_view alias,
        std::pmr::memory_resource* resource) const;

public:
#ifdef RUVIA_ENABLE_MARIADB
    using MariaDbPoolOwner = std::unique_ptr<MariaDbPool, PmrObjectDeleter<MariaDbPool>>;
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    using PostgreSqlPoolOwner = std::unique_ptr<PostgreSqlPool, PmrObjectDeleter<PostgreSqlPool>>;
#endif

#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using PoolOwner = std::variant<std::monostate, MariaDbPoolOwner, PostgreSqlPoolOwner>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using PoolOwner = std::variant<std::monostate, MariaDbPoolOwner>;
#else
    using PoolOwner = std::variant<std::monostate, PostgreSqlPoolOwner>;
#endif

    struct Entry {
        std::pmr::string alias;
        PoolOwner client;
    };

private:
    std::pmr::memory_resource* resource_;
    std::pmr::vector<Entry> clients_;
    DbPoolRef defaultClient_{};
};

}  // namespace ruvia::detail

#endif
