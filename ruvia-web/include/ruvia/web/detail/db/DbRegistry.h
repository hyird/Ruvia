#pragma once

#include "ruvia/web/detail/db/DbPoolOperations.h"

#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbBackend.h"

#if !defined(RUVIA_ENABLE_MARIADB) && !defined(RUVIA_ENABLE_POSTGRESQL)

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <memory_resource>
#include <span>

namespace ruvia::detail {

class DbRegistry final {
public:
    DbRegistry(asio::io_context&, std::pmr::memory_resource*, std::span<const DbDefinition>) {}

    DbRegistry(const DbRegistry&) = delete;
    DbRegistry& operator=(const DbRegistry&) = delete;

    [[nodiscard]] Task<void> connect() {
        co_return;
    }
    void closeNow() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
    void scanDeadlines() noexcept {}
    [[nodiscard]] bool hasAnyTimeout() const noexcept {
        return false;
    }
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
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/pool/PoolLeaseScheduler.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/db/DbHostResolution.h"

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
    MariaDbPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource = nullptr);
    ~MariaDbPool();

    MariaDbPool(const MariaDbPool&) = delete;
    MariaDbPool& operator=(const MariaDbPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;

public:
    template <typename Pool>
    friend Task<void> finishDbTransaction(Pool&, std::size_t, std::string_view, std::pmr::memory_resource*);
    template <typename Pool>
    friend Task<QueryResult> executeDbQuery(Pool&, std::pmr::string, std::pmr::vector<DbValue>, std::pmr::memory_resource*);
    template <typename Pool, typename Slot>
    friend Task<DbResolvedAddresses> resolveDbHost(Pool&, Slot&, const OperationTimeout&, std::string_view);
    template <typename Pool>
    friend Task<QueryResult> executeOnDbTransactionSlot(Pool&, std::size_t, std::pmr::string, std::pmr::vector<DbValue>, std::pmr::memory_resource*);
    template <typename Pool>
    friend Task<std::size_t> acquireDbSlot(Pool&);
    template <typename Pool>
    friend void releaseDbSlot(Pool&, std::size_t) noexcept;
    friend class ::ruvia::DbHandle;
    friend class ::ruvia::DbTransaction;
    friend class ::ruvia::DbStreamResult;

    using SlotGuard = DbSlotGuard<MariaDbPool>;

    struct ConnectionSlot {
        ConnectionSlot(asio::io_context& ioContext, std::pmr::memory_resource* resource = nullptr);
        ~ConnectionSlot();
        ConnectionSlot(ConnectionSlot&&) noexcept;
        ConnectionSlot& operator=(ConnectionSlot&&) noexcept;

        using SlotSocketDeleter = PmrObjectDeleter<DbSlotSocket>;

        asio::ip::tcp::resolver resolver;
        st_mysql* connection{nullptr};
        std::unique_ptr<DbSlotSocket, SlotSocketDeleter> waitSocket;
        std::coroutine_handle<> deadlineContinuation{};
        bool connected{false};
        // Shutdown may request closure while an async descriptor wait still
        // borrows both this slot and waitSocket. Keep the transport owner alive
        // until that wait resumes and the driving coroutine performs the final
        // close; destroying it immediately would leave the queued Asio handler
        // with dangling references.
        bool waitActive{false};
        bool closeRequested{false};
        enum class DeadlineKind : std::uint8_t { kResolve, kSocket, kSleep };
        OperationDeadline<DeadlineKind> deadline;
    };

public:
    // Backend dispatch entry points. The class itself remains detail-only.
    Task<std::size_t> acquireSlot();
    void releaseSlot(std::size_t slot) noexcept;
    void closeSlot(ConnectionSlot& slot) noexcept;
    void setSlotDeadline(ConnectionSlot& slot, std::chrono::milliseconds timeout, ConnectionSlot::DeadlineKind kind) noexcept;
    void clearSlotDeadline(ConnectionSlot& slot) noexcept;
    Task<DbResolvedAddresses> resolveHost(ConnectionSlot& slot, const OperationTimeout& deadline);
    Task<void> connectUnlocked(ConnectionSlot& slot);
    Task<int> waitForMysql(ConnectionSlot& slot, int status, const OperationTimeout& deadline);
    Task<void> runMysqlQuery(ConnectionSlot& slot, std::string_view sql, const OperationTimeout& deadline);
    Task<st_mysql_res*> storeMysqlResult(ConnectionSlot& slot, const OperationTimeout& deadline);
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
    PoolLeaseScheduler scheduler_;
};

#endif  // RUVIA_ENABLE_MARIADB

#ifdef RUVIA_ENABLE_POSTGRESQL

class PostgreSqlPool final {
public:
    PostgreSqlPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource = nullptr);
    ~PostgreSqlPool();

    PostgreSqlPool(const PostgreSqlPool&) = delete;
    PostgreSqlPool& operator=(const PostgreSqlPool&) = delete;

    Task<void> connect();
    void closeNow() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool hasAnyTimeout() const noexcept;

private:
    template <typename Pool>
    friend Task<void> finishDbTransaction(Pool&, std::size_t, std::string_view, std::pmr::memory_resource*);
    template <typename Pool>
    friend Task<QueryResult> executeDbQuery(Pool&, std::pmr::string, std::pmr::vector<DbValue>, std::pmr::memory_resource*);
    template <typename Pool, typename Slot>
    friend Task<DbResolvedAddresses> resolveDbHost(Pool&, Slot&, const OperationTimeout&, std::string_view);
    template <typename Pool>
    friend Task<QueryResult> executeOnDbTransactionSlot(Pool&, std::size_t, std::pmr::string, std::pmr::vector<DbValue>, std::pmr::memory_resource*);
    template <typename Pool>
    friend Task<std::size_t> acquireDbSlot(Pool&);
    template <typename Pool>
    friend void releaseDbSlot(Pool&, std::size_t) noexcept;
    friend class ::ruvia::DbHandle;
    friend class ::ruvia::DbTransaction;
    friend class ::ruvia::DbStreamResult;

    using SlotGuard = DbSlotGuard<PostgreSqlPool>;

    struct ConnectionSlot {
        ConnectionSlot(asio::io_context& ioContext, std::pmr::memory_resource* resource = nullptr);
        ~ConnectionSlot();
        ConnectionSlot(ConnectionSlot&&) noexcept;
        ConnectionSlot& operator=(ConnectionSlot&&) noexcept;

        using SlotSocketDeleter = PmrObjectDeleter<DbSlotSocket>;

        asio::ip::tcp::resolver resolver;
        pg_conn* connection{nullptr};
        std::unique_ptr<DbSlotSocket, SlotSocketDeleter> waitSocket;
        bool connected{false};
        bool waitActive{false};
        bool closeRequested{false};
        enum class DeadlineKind : std::uint8_t { kResolve, kSocket };
        OperationDeadline<DeadlineKind> deadline;
    };

public:
    // Backend dispatch entry points. The class itself remains detail-only.
    Task<std::size_t> acquireSlot();
    void releaseSlot(std::size_t slot) noexcept;
    void closeSlot(ConnectionSlot& slot) noexcept;
    void setSlotDeadline(ConnectionSlot& slot, std::optional<std::chrono::milliseconds> timeout) noexcept;
    void clearSlotDeadline(ConnectionSlot& slot) noexcept;
    Task<DbResolvedAddresses> resolveHost(ConnectionSlot& slot, const OperationTimeout& deadline);
    Task<void> connectUnlocked(ConnectionSlot& slot);
    Task<void> waitForPostgreSql(ConnectionSlot& slot, bool read, const OperationTimeout& deadline);
    Task<void> flushOutput(ConnectionSlot& slot, const OperationTimeout& deadline);
    Task<void> waitUntilResultReady(ConnectionSlot& slot, const OperationTimeout& deadline);
    Task<void> sendQuery(ConnectionSlot& slot, const std::pmr::string& sql, std::span<const DbValue> params, const OperationTimeout& deadline, bool singleRow);
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
    PoolLeaseScheduler scheduler_;
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
    [[nodiscard]] DbHandle get(std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const;
    [[nodiscard]] DbHandle get(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& operationScope) const;

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
    std::optional<std::size_t> defaultClientIndex_;
};

}  // namespace ruvia::detail

#endif
