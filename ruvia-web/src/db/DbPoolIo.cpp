#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbMysqlRuntime.h"
#include "ruvia/web/detail/db/DbPoolOperations.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbSql.h"

#include <mysql/mysql.h>

#include <array>
#include <charconv>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace ruvia {

Task<detail::DbResolvedAddresses> detail::MariaDbPool::resolveHost(ConnectionSlot& slot, const OperationTimeout& deadline) {
    return resolveDbHost(*this, slot, deadline, "MariaDB");
}

Task<void> detail::MariaDbPool::connectUnlocked(ConnectionSlot& slot) {
    if (slot.connected) {
        co_return;
    }
    OperationTimeout deadline(config_.connectTimeout);
    try {
        auto addresses = co_await resolveHost(slot, deadline);
        auto resolvedHosts = detail::makeMariaDbResolvedHostList(addresses, resource_);

        detail::ensureMysqlThreadInitialized();
        auto* connection = mysql_init(nullptr);
        if (connection == nullptr) {
            throw std::runtime_error("mysql_init failed");
        }
        slot.connection = connection;
        slot.waitSocket = detail::makePmrObject<detail::DbSlotSocket>(resource_, ioContext_);
        if (mysql_optionsv(slot.connection, MYSQL_OPT_NONBLOCK, nullptr) != 0) {
            throw mysqlError(*slot.connection, "enabling MariaDB non-blocking I/O");
        }
        // Pin the connection charset before connecting. mysql_real_escape_string --
        // the interpolateSql injection defense -- escapes according to the
        // connection charset, and is bypassable on multibyte charsets where a valid
        // character can end in 0x5C (GBK/Big5/SJIS, the classic backslash-swallowing
        // injection). utf8mb4 keeps 0x5C meaning only backslash, so escaping stays
        // safe regardless of the server or client library default, and it carries
        // full Unicode. Set here (not via SET NAMES) so it also governs the handshake.
        if (mysql_optionsv(slot.connection, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
            throw mysqlError(*slot.connection, "configuring MariaDB utf8mb4");
        }
        if (!detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_CONNECT_TIMEOUT, config_.connectTimeout) || !detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_READ_TIMEOUT, config_.readTimeout) || !detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_WRITE_TIMEOUT, config_.writeTimeout)) {
            throw mysqlError(*slot.connection, "configuring MariaDB timeouts");
        }

        auto& initialized = *slot.connection;
        constexpr auto clientFlags = 0UL;
        MYSQL* connected = nullptr;
        int status = mysql_real_connect_start(&connected, &initialized, resolvedHosts.c_str(), config_.username.c_str(), config_.password.c_str(), config_.database.empty() ? nullptr : config_.database.c_str(), config_.port, nullptr, clientFlags);

        while (status != 0) {
            status = mysql_real_connect_cont(&connected, &initialized, co_await waitForMysql(slot, status, deadline));
        }

        if (connected == nullptr) {
            throw mysqlError(initialized, "mysql_real_connect");
        }

        // MariaDB's non-blocking API suspends by yielding out of a fibre when
        // the socket reports EAGAIN. Connect leaves that socket in blocking
        // mode, so every later mysql_*_start() ran the entire statement inside
        // the call instead of returning a wait mask: the worker's event loop
        // stopped for as long as the query took -- no other connection on it
        // progressed, no timer fired, and no deadline could be enforced,
        // because nothing was running to enforce it. Switching the descriptor
        // restores the suspend the API is built around.
        const auto native = mysql_get_socket(&initialized);
        using NativeSocket = std::remove_cv_t<decltype(native)>;
        if (native == static_cast<NativeSocket>(MARIADB_INVALID_SOCKET) || slot.waitSocket == nullptr || !slot.waitSocket->ensureAssigned(static_cast<detail::DbSlotSocket::NativeSocket>(native)) || !slot.waitSocket->makeNonBlocking()) {
            throw std::runtime_error("MariaDB connection socket could not be made non-blocking");
        }
        slot.connected = true;
    } catch (...) {
        closeSlot(slot);
        throw;
    }
}

Task<void> detail::MariaDbPool::runMysqlQuery(ConnectionSlot& slot, std::string_view sql, const OperationTimeout& deadline) {
    auto& connection = *slot.connection;
    validateMariaDbSqlLength(sql.size());
    int queryResult = 0;
    int status = mysql_real_query_start(&queryResult, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(&queryResult, &connection, co_await waitForMysql(slot, status, deadline));
    }
    if (queryResult != 0) {
        throw mysqlError(connection, "mysql_real_query");
    }
    co_return;
}

Task<st_mysql_res*> detail::MariaDbPool::storeMysqlResult(ConnectionSlot& slot, const OperationTimeout& deadline) {
    auto& connection = *slot.connection;
    MYSQL_RES* result = nullptr;
    int status = mysql_store_result_start(&result, &connection);
    while (status != 0) {
        status = mysql_store_result_cont(&result, &connection, co_await waitForMysql(slot, status, deadline));
    }
    co_return result;
}

Task<int> detail::MariaDbPool::waitForMysql(ConnectionSlot& slot, int status, const OperationTimeout& deadline) {
    auto& connection = *slot.connection;
    const auto timeout = deadline.remaining();
    // A DbConfig deadline is this pool's to enforce, not the driver's. Reporting
    // its expiry to MariaDB as MYSQL_WAIT_TIMEOUT only works where libmariadb
    // has a matching timeout option of its own -- it has one for connect, none
    // for a statement -- so a query deadline was handed over and dropped, and
    // the wait was simply re-entered until the server answered. Failing here
    // instead ends the operation, and the caller closes the connection: the
    // statement may still be running server-side, which is exactly what a
    // client-side timeout means.
    const auto timedOut = [] { return std::runtime_error("MariaDB operation timed out"); };
    if (timeout.has_value() && timeout->count() <= 0) {
        throw timedOut();
    }
    const auto wantsRead = (status & MYSQL_WAIT_READ) != 0;
    const auto wantsWrite = (status & MYSQL_WAIT_WRITE) != 0;
    const auto wantsException = (status & MYSQL_WAIT_EXCEPT) != 0;
    if (!wantsRead && !wantsWrite && !wantsException) {
        auto timeoutMs = timeout.value_or(std::chrono::milliseconds(0));
        if (timeoutMs.count() <= 0) {
            const auto mysqlTimeout = mysql_get_timeout_value_ms(&connection);
            timeoutMs = std::chrono::milliseconds(mysqlTimeout == 0 ? 1 : mysqlTimeout);
        }
        setSlotDeadline(slot, timeoutMs, ConnectionSlot::DeadlineKind::kSleep);
        struct DeadlineAwaiter final {
            ConnectionSlot& slot;

            [[nodiscard]] bool await_ready() const noexcept {
                return slot.deadline.expired();
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                slot.deadlineContinuation = handle;
            }

            void await_resume() const noexcept {}
        };
        struct ActiveWait final {
            explicit ActiveWait(ConnectionSlot& value) noexcept
                : slot(value) {
                if (slot.waitActive) {
                    std::terminate();
                }
                slot.waitActive = true;
            }

            ~ActiveWait() {
                slot.waitActive = false;
            }

            ConnectionSlot& slot;
        } activeWait(slot);
        co_await DeadlineAwaiter{slot};
        clearSlotDeadline(slot);
        if (slot.closeRequested) {
            throw std::runtime_error("database client is closing");
        }
        co_return MYSQL_WAIT_TIMEOUT;
    }

    // Whose deadline governs this wait decides what its expiry means: ours ends
    // the operation, one MariaDB asked for is an event it is waiting to be told
    // about.
    auto timeoutMs = timeout.value_or(std::chrono::milliseconds(0));
    const bool ownDeadline = timeout.has_value();
    if (timeoutMs.count() <= 0 && (status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto mysqlTimeout = mysql_get_timeout_value_ms(&connection);
        timeoutMs = std::chrono::milliseconds(mysqlTimeout == 0 ? 1 : mysqlTimeout);
    }

    const auto native = mysql_get_socket(&connection);
    using NativeSocket = std::remove_cv_t<decltype(native)>;
    if (native == static_cast<NativeSocket>(MARIADB_INVALID_SOCKET)) {
        co_return MYSQL_WAIT_TIMEOUT;
    }

    if (slot.waitSocket == nullptr || !slot.waitSocket->ensureAssigned(static_cast<detail::DbSlotSocket::NativeSocket>(native))) {
        co_return MYSQL_WAIT_EXCEPT;
    }

    setSlotDeadline(slot, timeoutMs, ConnectionSlot::DeadlineKind::kSocket);
    struct SocketWaitAwaiter final {
        ConnectionSlot& slot;
        detail::DbSlotSocket& slotSocket;
        int status;
        std::coroutine_handle<> continuation{};
        int result{MYSQL_WAIT_TIMEOUT};
        int pending{0};
        bool resultSet{false};
        std::exception_ptr initiationFailure;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept {
            continuation = handle;
#if defined(_WIN32)
            auto& waitable = slotSocket.socket;
            constexpr auto readWait = asio::ip::tcp::socket::wait_read;
            constexpr auto writeWait = asio::ip::tcp::socket::wait_write;
            constexpr auto errorWait = asio::ip::tcp::socket::wait_error;
#else
            auto& waitable = slotSocket.descriptor;
            constexpr auto readWait = asio::posix::stream_descriptor::wait_read;
            constexpr auto writeWait = asio::posix::stream_descriptor::wait_write;
            constexpr auto errorWait = asio::posix::stream_descriptor::wait_error;
#endif

            try {
                // MariaDB returns a bitmask and _cont() accepts the events that
                // actually occurred, so every requested readiness class must
                // participate in this any-of wait. Increment only after Asio
                // accepts an operation: if a later initiation throws, cancel
                // and stay suspended until the already-published handlers have
                // drained before surfacing the original exception.
                if ((status & MYSQL_WAIT_READ) != 0) {
                    waitable.async_wait(readWait, [this](std::error_code waitEc) noexcept { onSocket(MYSQL_WAIT_READ, waitEc); });
                    ++pending;
                }
                if ((status & MYSQL_WAIT_WRITE) != 0) {
                    waitable.async_wait(writeWait, [this](std::error_code waitEc) noexcept { onSocket(MYSQL_WAIT_WRITE, waitEc); });
                    ++pending;
                }
                if ((status & MYSQL_WAIT_EXCEPT) != 0) {
                    waitable.async_wait(errorWait, [this](std::error_code waitEc) noexcept { onSocket(MYSQL_WAIT_EXCEPT, waitEc); });
                    ++pending;
                }
            } catch (...) {
                initiationFailure = std::current_exception();
                resultSet = true;
                slotSocket.cancel();
            }
            return pending != 0;
        }

        [[nodiscard]] int await_resume() const {
            if (initiationFailure != nullptr) {
                std::rethrow_exception(initiationFailure);
            }
            return result;
        }

        void onSocket(int flag, std::error_code) noexcept {
            if (!resultSet) {
                result = slot.deadline.expired() ? MYSQL_WAIT_TIMEOUT : flag;
                resultSet = true;
                slotSocket.cancel();
            }
            finishOne();
        }

        void finishOne() noexcept {
            --pending;
            if (pending == 0 && continuation) {
                continuation.resume();
            }
        }
    };

    struct ActiveWait final {
        explicit ActiveWait(ConnectionSlot& value) noexcept
            : slot(value) {
            if (slot.waitActive) {
                std::terminate();
            }
            slot.waitActive = true;
        }

        ~ActiveWait() {
            slot.waitActive = false;
        }

        ConnectionSlot& slot;
    } activeWait(slot);
    const auto result = co_await SocketWaitAwaiter{slot, *slot.waitSocket, status, {}, MYSQL_WAIT_TIMEOUT, 0, false, {}};
    const bool expired = slot.deadline.expired();
    clearSlotDeadline(slot);
    if (slot.closeRequested) {
        throw std::runtime_error("database client is closing");
    }
    if (ownDeadline && expired) {
        throw timedOut();
    }
    co_return result;
}

}  // namespace ruvia
