#include "ruvia/web/detail/db/DbRegistry.h"

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/db/DbPoolOperations.h"
#include "ruvia/web/detail/db/DbPostgreSql.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia::detail {

Task<DbResolvedAddresses> PostgreSqlPool::resolveHost(
    ConnectionSlot& slot,
    const OperationTimeout& deadline) {
    return resolveDbHost(*this, slot, deadline, "PostgreSQL");
}

Task<void> PostgreSqlPool::connectUnlocked(ConnectionSlot& slot) {
    if (slot.connected) {
        co_return;
    }
    OperationTimeout deadline(config_.connectTimeout);
    try {
        auto addresses = co_await resolveHost(slot, deadline);
        auto resolvedHosts = makePostgreSqlResolvedHostList(
            config_.host,
            addresses,
            resource_);
        const auto port = formatDbPort(config_.port, "PostgreSQL");
        // Pin the client encoding to UTF-8. Ruvia's strings are UTF-8 throughout,
        // and query parameters are sent in text format; without this the connection
        // inherits the server/database default encoding, so non-ASCII parameters and
        // result text would be misinterpreted on a non-UTF-8 database (e.g. LATIN1,
        // SQL_ASCII). libpq accepts client_encoding as a connection keyword.
        const std::array<const char*, 8> keywords{
            "host", "hostaddr", "port", "user", "password", "dbname",
            "client_encoding", nullptr};
        const std::array<const char*, 8> values{
            resolvedHosts.hosts.c_str(),
            resolvedHosts.addresses.c_str(),
            port.data(),
            config_.username.c_str(),
            config_.password.c_str(),
            config_.database.c_str(),
            "UTF8",
            nullptr};
        slot.connection = PQconnectStartParams(keywords.data(), values.data(), 0);
        if (slot.connection == nullptr) {
            throw std::runtime_error("PQconnectStartParams failed");
        }
        slot.waitSocket = makePmrObject<DbSlotSocket>(resource_, ioContext_);
        if (PQstatus(slot.connection) == CONNECTION_BAD) {
            throw postgreSqlError(*slot.connection, "PQconnectStartParams");
        }

        auto status = PQconnectPoll(slot.connection);
        while (status == PGRES_POLLING_READING ||
               status == PGRES_POLLING_WRITING ||
               status == PGRES_POLLING_ACTIVE) {
            if (status == PGRES_POLLING_ACTIVE) {
                status = PQconnectPoll(slot.connection);
                continue;
            }
            co_await waitForPostgreSql(
                slot,
                status == PGRES_POLLING_READING,
                deadline);
            status = PQconnectPoll(slot.connection);
        }
        if (status != PGRES_POLLING_OK ||
            PQstatus(slot.connection) != CONNECTION_OK) {
            throw postgreSqlError(*slot.connection, "PQconnectPoll");
        }
        if (PQsetnonblocking(slot.connection, 1) != 0) {
            throw postgreSqlError(*slot.connection, "PQsetnonblocking");
        }
        slot.connected = true;
    } catch (...) {
        closeSlot(slot);
        throw;
    }
}

Task<void> PostgreSqlPool::waitForPostgreSql(
    ConnectionSlot& slot,
    bool read,
    const OperationTimeout& deadline) {
    const auto remaining = deadline.remaining();
    if (remaining.has_value() && remaining->count() <= 0) {
        throw std::runtime_error("PostgreSQL operation timed out");
    }
    const auto native = PQsocket(slot.connection);
    if (native < 0 || slot.waitSocket == nullptr ||
        !slot.waitSocket->ensureAssigned(
            static_cast<DbSlotSocket::NativeSocket>(native))) {
        throw std::runtime_error("PostgreSQL connection socket is unavailable");
    }

    setSlotDeadline(slot, remaining);
    struct SocketWaitAwaiter final {
        ConnectionSlot& slot;
        DbSlotSocket& socket;
        bool read;
        std::coroutine_handle<> continuation{};
        std::error_code error;
        std::exception_ptr initiationFailure;

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        [[nodiscard]] bool await_suspend(
            std::coroutine_handle<> handle) noexcept {
            continuation = handle;
#if defined(_WIN32)
            auto& waitable = socket.socket;
            const auto waitType = read
                ? asio::ip::tcp::socket::wait_read
                : asio::ip::tcp::socket::wait_write;
#else
            auto& waitable = socket.descriptor;
            const auto waitType = read
                ? asio::posix::stream_descriptor::wait_read
                : asio::posix::stream_descriptor::wait_write;
#endif
            try {
                waitable.async_wait(
                    waitType,
                    [this](std::error_code waitError) noexcept {
                        error = waitError;
                        continuation.resume();
                    });
                return true;
            } catch (...) {
                initiationFailure = std::current_exception();
                return false;
            }
        }

        void await_resume() const {
            if (initiationFailure != nullptr) {
                std::rethrow_exception(initiationFailure);
            }
            if (slot.closeRequested) {
                throw std::runtime_error("database client is closing");
            }
            if (slot.deadline.expired()) {
                throw std::runtime_error("PostgreSQL operation timed out");
            }
            if (error) {
                throw std::system_error(error, "PostgreSQL socket wait failed");
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

        ~ActiveWait() { slot.waitActive = false; }

        ConnectionSlot& slot;
    } activeWait(slot);
    try {
        co_await SocketWaitAwaiter{
            slot, *slot.waitSocket, read, {}, {}, {}};
    } catch (...) {
        clearSlotDeadline(slot);
        throw;
    }
    clearSlotDeadline(slot);
}

Task<void> PostgreSqlPool::flushOutput(
    ConnectionSlot& slot,
    const OperationTimeout& deadline) {
    while (true) {
        const auto status = PQflush(slot.connection);
        if (status == 0) {
            co_return;
        }
        if (status < 0) {
            throw postgreSqlError(*slot.connection, "PQflush");
        }
        co_await waitForPostgreSql(slot, false, deadline);
    }
}

Task<void> PostgreSqlPool::waitUntilResultReady(
    ConnectionSlot& slot,
    const OperationTimeout& deadline) {
    while (PQisBusy(slot.connection) != 0) {
        co_await waitForPostgreSql(slot, true, deadline);
        if (PQconsumeInput(slot.connection) == 0) {
            throw postgreSqlError(*slot.connection, "PQconsumeInput");
        }
    }
}

Task<void> PostgreSqlPool::sendQuery(
    ConnectionSlot& slot,
    const std::pmr::string& sql,
    std::span<const DbValue> params,
    const OperationTimeout& deadline,
    bool singleRow) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    if (!std::in_range<int>(params.size())) {
        throw std::invalid_argument("too many PostgreSQL query parameters");
    }
    auto encoded = encodePostgreSqlParams(params, resource_);
    const auto* values = encoded.values.empty() ? nullptr : encoded.values.data();
    if (PQsendQueryParams(
            slot.connection,
            sql.c_str(),
            static_cast<int>(params.size()),
            nullptr,
            values,
            nullptr,
            nullptr,
            0) == 0) {
        throw postgreSqlError(*slot.connection, "PQsendQueryParams");
    }
    if (singleRow && PQsetSingleRowMode(slot.connection) == 0) {
        throw postgreSqlError(*slot.connection, "PQsetSingleRowMode");
    }
    co_await flushOutput(slot, deadline);
}

}  // namespace ruvia::detail
