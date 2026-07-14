#include "ruvia/web/detail/db/DbPoolDeadline.h"
#include "ruvia/web/detail/db/DbMysqlRuntime.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbSql.h"

#include <mysql/mysql.h>

#include <coroutine>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace ruvia {

Task<void> detail::MariaDbPool::connectUnlocked(ConnectionSlot& slot) {
    if (slot.connected) {
        co_return;
    }

    detail::ensureMysqlThreadInitialized();
    if (slot.connection == nullptr) {
        auto* connection = mysql_init(nullptr);
        if (connection == nullptr) {
            throw std::runtime_error("mysql_init failed");
        }
        slot.connection = connection;
        slot.waitSocket = detail::makePmrObject<detail::SlotSocket>(resource_, ioContext_);
        constexpr std::size_t kMysqlAsyncStackBytes = 1024 * 1024;
        (void)mysql_options(slot.connection, MYSQL_OPT_NONBLOCK, &kMysqlAsyncStackBytes);
        // Pin the connection charset before connecting. mysql_real_escape_string --
        // the interpolateSql injection defense -- escapes according to the
        // connection charset, and is bypassable on multibyte charsets where a valid
        // character can end in 0x5C (GBK/Big5/SJIS, the classic backslash-swallowing
        // injection). utf8mb4 keeps 0x5C meaning only backslash, so escaping stays
        // safe regardless of the server or client library default, and it carries
        // full Unicode. Set here (not via SET NAMES) so it also governs the handshake.
        (void)mysql_options(slot.connection, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_CONNECT_TIMEOUT, config_.connectTimeout);
        detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_READ_TIMEOUT, config_.readTimeout);
        detail::setMysqlTimeout(*slot.connection, MYSQL_OPT_WRITE_TIMEOUT, config_.writeTimeout);
    }

    auto& connection = *slot.connection;
    constexpr auto clientFlags = 0UL;
    MYSQL* connected = nullptr;
    OperationDeadline deadline(config_.connectTimeout);
    int status = mysql_real_connect_start(
        &connected,
        &connection,
        config_.host.c_str(),
        config_.username.c_str(),
        config_.password.c_str(),
        config_.database.empty() ? nullptr : config_.database.c_str(),
        config_.port,
        nullptr,
        clientFlags);

    while (status != 0) {
        status = mysql_real_connect_cont(
            &connected,
            &connection,
            co_await waitForMysql(slot, status, deadline));
    }

    if (connected == nullptr) {
        auto error = mysqlError(connection, "mysql_real_connect");
        closeSlot(slot);
        throw error;
    }

    slot.connected = true;
}

Task<void> detail::MariaDbPool::runMysqlQuery(
    ConnectionSlot& slot,
    std::string_view sql,
    const OperationDeadline& deadline) {
    auto& connection = *slot.connection;
    int queryResult = 0;
    int status = mysql_real_query_start(
        &queryResult,
        &connection,
        sql.data(),
        static_cast<unsigned long>(sql.size()));
    while (status != 0) {
        status = mysql_real_query_cont(
            &queryResult,
            &connection,
            co_await waitForMysql(slot, status, deadline));
    }
    if (queryResult != 0) {
        throw mysqlError(connection, "mysql_real_query");
    }
    co_return;
}

Task<st_mysql_res*> detail::MariaDbPool::storeMysqlResult(
    ConnectionSlot& slot,
    const OperationDeadline& deadline) {
    auto& connection = *slot.connection;
    MYSQL_RES* result = nullptr;
    int status = mysql_store_result_start(&result, &connection);
    while (status != 0) {
        status = mysql_store_result_cont(
            &result,
            &connection,
            co_await waitForMysql(slot, status, deadline));
    }
    co_return result;
}

Task<int> detail::MariaDbPool::waitForMysql(
    ConnectionSlot& slot,
    int status,
    const OperationDeadline& deadline) {
    auto& connection = *slot.connection;
    const auto timeout = deadline.remaining();
    if (timeout.has_value() && timeout->count() <= 0) {
        co_return MYSQL_WAIT_TIMEOUT;
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
                return slot.timedOut;
            }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                slot.deadlineContinuation = handle;
            }

            void await_resume() const noexcept {}
        };
        co_await DeadlineAwaiter{slot};
        clearSlotDeadline(slot);
        co_return MYSQL_WAIT_TIMEOUT;
    }

    auto timeoutMs = timeout.value_or(std::chrono::milliseconds(0));
    if (timeoutMs.count() <= 0 && (status & MYSQL_WAIT_TIMEOUT) != 0) {
        const auto mysqlTimeout = mysql_get_timeout_value_ms(&connection);
        timeoutMs = std::chrono::milliseconds(mysqlTimeout == 0 ? 1 : mysqlTimeout);
    }

    const auto native = mysql_get_socket(&connection);
    using NativeSocket = std::remove_cv_t<decltype(native)>;
    if (native == static_cast<NativeSocket>(MARIADB_INVALID_SOCKET)) {
        co_return MYSQL_WAIT_TIMEOUT;
    }

    if (slot.waitSocket == nullptr || !slot.waitSocket->ensureAssigned(native)) {
        co_return MYSQL_WAIT_EXCEPT;
    }

    setSlotDeadline(slot, timeoutMs, ConnectionSlot::DeadlineKind::kSocket);
    struct SocketWaitAwaiter final {
        ConnectionSlot& slot;
        detail::SlotSocket& slotSocket;
        int status;
        std::coroutine_handle<> continuation{};
        int result{MYSQL_WAIT_TIMEOUT};
        int pending{0};
        bool resultSet{false};

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

            if ((status & MYSQL_WAIT_READ) != 0) {
                ++pending;
                waitable.async_wait(readWait, [this](std::error_code waitEc) noexcept {
                    onSocket(MYSQL_WAIT_READ, waitEc);
                });
            }
            if ((status & MYSQL_WAIT_WRITE) != 0) {
                ++pending;
                waitable.async_wait(writeWait, [this](std::error_code waitEc) noexcept {
                    onSocket(MYSQL_WAIT_WRITE, waitEc);
                });
            }
            if ((status & MYSQL_WAIT_EXCEPT) != 0) {
                ++pending;
                waitable.async_wait(errorWait, [this](std::error_code waitEc) noexcept {
                    onSocket(MYSQL_WAIT_EXCEPT, waitEc);
                });
            }
            return true;
        }

        [[nodiscard]] int await_resume() const noexcept {
            return result;
        }

        void onSocket(int flag, std::error_code) noexcept {
            if (!resultSet) {
                result = slot.timedOut ? MYSQL_WAIT_TIMEOUT : flag;
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

    const auto result = co_await SocketWaitAwaiter{slot, *slot.waitSocket, status};
    clearSlotDeadline(slot);
    co_return result;
}

}  // namespace ruvia
