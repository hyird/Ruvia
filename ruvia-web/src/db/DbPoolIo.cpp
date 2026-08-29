#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbMysqlRuntime.h"
#include "ruvia/web/detail/db/DbPoolOperations.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbSql.h"

#include <mysql/mysql.h>

#include <array>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ruvia {

namespace {

[[nodiscard]] DbError mysqlSocketError(std::string_view operation, std::error_code error) {
    std::string message(operation);
    if (error) {
        message.append(": ");
        message.append(error.message());
    }
    return DbError(DbError::Code::kIoError, DbDriver::kMariaDb, std::move(message),
        error ? std::optional<std::int64_t>(error.value()) : std::nullopt);
}

}  // namespace

Task<detail::DbResolvedAddresses> detail::MariaDbPool::resolveHost(
    ConnectionSlot& slot, const OperationTimeout& deadline) {
    return resolveDbHost(*this, slot, deadline, "MariaDB");
}

Task<void> detail::MariaDbPool::connectUnlocked(
    ConnectionSlot& slot, const OperationTimeout& operationTimeout) {
    if (scheduler_.closing()) {
        throw DbError(DbError::Code::kClosing, DbDriver::kMariaDb, "database client is closing");
    }
    if (slot.connected) {
        co_return;
    }
    throwIfCancelled(slot);
    const OperationTimeout deadline = operationTimeout.constrainedBy(config_.connectTimeout);
    try {
        auto addresses = co_await resolveHost(slot, deadline);
        auto resolvedHosts = detail::makeMariaDbResolvedHostList(addresses, resource_);

        detail::ensureMysqlThreadInitialized();
        auto* connection = mysql_init(nullptr);
        if (connection == nullptr) {
            throw DbError(DbError::Code::kConnectFailed, DbDriver::kMariaDb, "mysql_init failed");
        }
        slot.connection = connection;
        slot.waitSocket = detail::makePmrObject<detail::DbSlotSocket>(resource_, ioContext_);
        if (mysql_optionsv(slot.connection, MYSQL_OPT_NONBLOCK, nullptr) != 0) {
            throw mysqlError(*slot.connection, "enabling MariaDB non-blocking I/O",
                DbError::Code::kConnectFailed);
        }
        // Pin the connection charset before connecting. mysql_real_escape_string --
        // the interpolateSql injection defense -- escapes according to the
        // connection charset, and is bypassable on multibyte charsets where a valid
        // character can end in 0x5C (GBK/Big5/SJIS, the classic backslash-swallowing
        // injection). utf8mb4 keeps 0x5C meaning only backslash, so escaping stays
        // safe regardless of the server or client library default, and it carries
        // full Unicode. Set here (not via SET NAMES) so it also governs the handshake.
        if (mysql_optionsv(slot.connection, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
            throw mysqlError(
                *slot.connection, "configuring MariaDB utf8mb4", DbError::Code::kConnectFailed);
        }
        if (!detail::setMysqlTimeout(
                *slot.connection, MYSQL_OPT_CONNECT_TIMEOUT, config_.connectTimeout) ||
            !detail::setMysqlTimeout(
                *slot.connection, MYSQL_OPT_READ_TIMEOUT, config_.readTimeout) ||
            !detail::setMysqlTimeout(
                *slot.connection, MYSQL_OPT_WRITE_TIMEOUT, config_.writeTimeout)) {
            throw mysqlError(
                *slot.connection, "configuring MariaDB timeouts", DbError::Code::kConnectFailed);
        }

        auto& initialized = *slot.connection;
        constexpr auto clientFlags = 0UL;
        MYSQL* connected = nullptr;
        int status = mysql_real_connect_start(&connected, &initialized, resolvedHosts.c_str(),
            config_.username.c_str(), config_.password.c_str(),
            config_.database.empty() ? nullptr : config_.database.c_str(), config_.port, nullptr,
            clientFlags);

        while (status != 0) {
            status = mysql_real_connect_cont(
                &connected, &initialized, co_await waitForMysql(slot, status, deadline));
        }

        if (connected == nullptr) {
            throw mysqlError(initialized, "mysql_real_connect", DbError::Code::kConnectFailed);
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
        if (native == static_cast<NativeSocket>(MARIADB_INVALID_SOCKET) ||
            slot.waitSocket == nullptr) {
            throw mysqlSocketError("MariaDB connection has no waitable socket", {});
        }
        if (const auto error = slot.waitSocket->ensureAssigned(
                static_cast<detail::DbSlotSocket::NativeSocket>(native));
            error) {
            throw mysqlSocketError("binding MariaDB connection socket", error);
        }
        const auto nonBlockingError = slot.waitSocket->makeNonBlocking();
        const auto releaseError = slot.waitSocket->release();
        if (nonBlockingError) {
            throw mysqlSocketError(
                "making MariaDB connection socket non-blocking", nonBlockingError);
        }
        if (releaseError) {
            throw mysqlSocketError("detaching MariaDB connection socket", releaseError);
        }
        slot.connected = true;
    } catch (...) {
        const auto failure = std::current_exception();
        closeSlot(slot);
        std::rethrow_exception(failure);
    }
}

Task<detail::OperationTimeout> detail::MariaDbPool::runMysqlStatement(ConnectionSlot& slot,
    std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource,
    const OperationTimeout& operationTimeout) {
    throwIfCancelled(slot);
    if (!slot.connected) {
        co_await connectUnlocked(slot, operationTimeout);
    }
    const OperationTimeout deadline = operationTimeout.constrainedBy(config_.queryTimeout);
    std::pmr::string interpolatedSql(detail::pmrResourceOrDefault(resource));
    if (!params.empty()) {
        interpolatedSql = interpolateSql(*slot.connection, sql, params, resource);
        sql = interpolatedSql;
    }

    auto& connection = *slot.connection;
    validateMariaDbSqlLength(sql.size());
    int queryResult = 0;
    int status = mysql_real_query_start(
        &queryResult, &connection, sql.data(), static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &queryResult, &connection, co_await waitForMysql(slot, status, deadline));
    }
    if (queryResult != 0) {
        throw mysqlError(connection, "mysql_real_query", DbError::Code::kStatementFailed);
    }
    co_return deadline;
}

Task<st_mysql_res*> detail::MariaDbPool::storeMysqlResult(
    ConnectionSlot& slot, const OperationTimeout& deadline) {
    auto& connection = *slot.connection;
    MYSQL_RES* result = nullptr;
    int status = mysql_store_result_start(&result, &connection);
    while (status != 0) {
        status = mysql_store_result_cont(
            &result, &connection, co_await waitForMysql(slot, status, deadline));
    }
    co_return result;
}

Task<int> detail::MariaDbPool::waitForMysql(
    ConnectionSlot& slot, int status, const OperationTimeout& deadline) {
    throwIfCancelled(slot);
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
    const auto timedOut = [] {
        return DbError(DbError::Code::kTimeout, DbDriver::kMariaDb, "MariaDB operation timed out");
    };
    if (timeout.has_value() && timeout->count() <= 0) {
        throw timedOut();
    }
    std::optional<std::chrono::milliseconds> driverTimeout;
    if ((status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto milliseconds = mysql_get_timeout_value_ms(&connection);
        driverTimeout = std::chrono::milliseconds(milliseconds == 0 ? 1 : milliseconds);
    }
    const auto selectedDeadline = detail::selectMysqlWaitDeadline(timeout, driverTimeout);
    const auto wantsRead = (status & MYSQL_WAIT_READ) != 0;
    const auto wantsWrite = (status & MYSQL_WAIT_WRITE) != 0;
    const auto wantsException = (status & MYSQL_WAIT_EXCEPT) != 0;
    if (!wantsRead && !wantsWrite && !wantsException) {
        if (!selectedDeadline.timeout) {
            throw DbError(DbError::Code::kIoError, DbDriver::kMariaDb,
                "MariaDB requested an unsupported empty wait");
        }
        setSlotDeadline(slot, *selectedDeadline.timeout, ConnectionSlot::DeadlineKind::kSleep);
        struct DeadlineAwaiter final {
            ConnectionSlot& slot;

            [[nodiscard]] bool await_ready() noexcept {
                return slot.deadline.expire(std::chrono::steady_clock::now()).has_value() ||
                       slot.deadline.expired();
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                slot.deadlineContinuation = handle;
            }

            void await_resume() const noexcept {}
        };
        DbSlotActiveWaitGuard activeWait(slot);
        co_await DeadlineAwaiter{slot};
        clearSlotDeadline(slot);
        throwIfCancelled(slot);
        if (slot.closeRequested) {
            throw DbError(
                DbError::Code::kClosing, DbDriver::kMariaDb, "database client is closing");
        }
        if (deadline.expired() ||
            selectedDeadline.source == detail::MysqlWaitDeadlineSource::kOperation) {
            throw timedOut();
        }
        co_return MYSQL_WAIT_TIMEOUT;
    }

    const auto native = mysql_get_socket(&connection);
    using NativeSocket = std::remove_cv_t<decltype(native)>;
    if (native == static_cast<NativeSocket>(MARIADB_INVALID_SOCKET)) {
        throw mysqlSocketError("MariaDB returned an invalid socket for an active wait",
            std::make_error_code(std::errc::bad_file_descriptor));
    }

    if (slot.waitSocket == nullptr) {
        throw mysqlSocketError("MariaDB wait socket is unavailable", {});
    }
    if (const auto error = slot.waitSocket->ensureAssigned(
            static_cast<detail::DbSlotSocket::NativeSocket>(native));
        error) {
        throw mysqlSocketError("binding MariaDB wait socket", error);
    }

    setSlotDeadline(slot, selectedDeadline.timeout.value_or(std::chrono::milliseconds(0)),
        ConnectionSlot::DeadlineKind::kSocket);
    struct SocketWaitAwaiter final {
        ConnectionSlot& slot;
        detail::DbSlotSocket& slotSocket;
        int status;
        std::error_code& socketFailure;
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
                    waitable.async_wait(readWait, [this](std::error_code waitEc) noexcept {
                        onSocket(MYSQL_WAIT_READ, waitEc);
                    });
                    ++pending;
                }
                if ((status & MYSQL_WAIT_WRITE) != 0) {
                    waitable.async_wait(writeWait, [this](std::error_code waitEc) noexcept {
                        onSocket(MYSQL_WAIT_WRITE, waitEc);
                    });
                    ++pending;
                }
                if ((status & MYSQL_WAIT_EXCEPT) != 0) {
                    waitable.async_wait(errorWait, [this](std::error_code waitEc) noexcept {
                        onSocket(MYSQL_WAIT_EXCEPT, waitEc);
                    });
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

        void onSocket(int flag, std::error_code waitError) noexcept {
            if (!resultSet) {
                const bool deadlineExpired =
                    slot.deadline.expire(std::chrono::steady_clock::now()).has_value() ||
                    slot.deadline.expired();
                if (deadlineExpired) {
                    result = MYSQL_WAIT_TIMEOUT;
                } else if (waitError) {
                    socketFailure = waitError;
                } else {
                    result = flag;
                }
                resultSet = true;
                slotSocket.cancel();
            }
            finishOne();
        }

        void finishOne() noexcept {
            --pending;
            if (pending == 0 && continuation) {
                // Asio releases each wait operation before invoking its user
                // handler. Earlier handlers have returned when pending reaches
                // zero, so no outstanding operation still borrows this awaiter.
                continuation.resume();
            }
        }
    };

    int result = MYSQL_WAIT_TIMEOUT;
    bool expired = false;
    std::error_code socketFailure;
    std::exception_ptr waitFailure;
    {
        DbSlotActiveWaitGuard activeWait(slot);
        try {
            result = co_await SocketWaitAwaiter{slot, *slot.waitSocket, status, socketFailure, {},
                MYSQL_WAIT_TIMEOUT, 0, false, {}};
            expired = slot.deadline.expired();
        } catch (...) {
            waitFailure = std::current_exception();
            expired = slot.deadline.expired();
        }
    }
    // The driver may reuse or close its socket in mysql_*_cont(). Detach ASIO
    // only after all readiness handlers have drained, and before returning
    // control to that continuation.
    const auto releaseError = slot.waitSocket->release();
    const bool operationExpired =
        deadline.expired() ||
        (selectedDeadline.source == detail::MysqlWaitDeadlineSource::kOperation && expired);
    clearSlotDeadline(slot);
    throwIfCancelled(slot);
    if (slot.closeRequested) {
        throw DbError(DbError::Code::kClosing, DbDriver::kMariaDb, "database client is closing");
    }
    if (operationExpired) {
        throw timedOut();
    }
    if (waitFailure != nullptr) {
        try {
            std::rethrow_exception(waitFailure);
        } catch (const DbError&) {
            throw;
        } catch (const std::system_error& error) {
            throw DbError(
                DbError::Code::kIoError, DbDriver::kMariaDb, error.what(), error.code().value());
        } catch (const std::runtime_error& error) {
            throw DbError(DbError::Code::kIoError, DbDriver::kMariaDb, error.what());
        }
    }
    if (socketFailure) {
        throw mysqlSocketError("waiting for MariaDB socket", socketFailure);
    }
    if (releaseError) {
        throw mysqlSocketError("detaching MariaDB wait socket", releaseError);
    }
    co_return result;
}

}  // namespace ruvia
