#include "../DbInternal.h"
#include "DbUtils.h"
#include "ruvia/http/Context.h"

#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#include <mysql/mysql.h>

#include "../../runtime/AsioAwait.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace ruvia {


namespace detail {

// Persistent ASIO wrapper around a single MariaDB connection socket.
//
// MariaDB hands us a native socket via mysql_get_socket(); we need ASIO to wait
// for readiness on it. On Windows the IOCP backend permanently associates a
// socket handle with the completion port when assign() is called, and release()
// cannot undo that association. Re-assigning the same fd on every wait therefore
// fails the second time (ERROR_INVALID_PARAMETER) and breaks the connection.
// To stay portable we assign the socket exactly once per connection here and
// reuse it for every subsequent wait.
struct SlotSocket final {
    explicit SlotSocket(asio::io_context& ioContext)
#if defined(_WIN32)
        : socket(ioContext) {}
    asio::ip::tcp::socket socket;
#else
        : descriptor(ioContext) {}
    asio::posix::stream_descriptor descriptor;
#endif
    my_socket native{static_cast<my_socket>(MARIADB_INVALID_SOCKET)};

    // Ensures the ASIO socket is bound to `fd`, assigning it on first use. Cheap
    // (no-op) once the fd is already assigned. Returns false if `fd` is invalid
    // or the assignment fails.
    [[nodiscard]] bool ensureAssigned(my_socket fd) noexcept {
        if (fd == static_cast<my_socket>(MARIADB_INVALID_SOCKET)) {
            return false;
        }
        std::error_code ec;
#if defined(_WIN32)
        if (socket.is_open()) {
            if (native == fd) {
                return true;
            }
            (void)socket.release(ec);
        }
        socket.assign(asio::ip::tcp::v4(), fd, ec);
#else
        if (descriptor.is_open()) {
            if (native == fd) {
                return true;
            }
            (void)descriptor.release();
        }
        descriptor.assign(fd, ec);
#endif
        if (ec) {
            native = static_cast<my_socket>(MARIADB_INVALID_SOCKET);
            return false;
        }
        native = fd;
        return true;
    }

    // Detaches the native socket from ASIO without closing it; MariaDB still owns
    // the fd and closes it via mysql_close().
    void release() noexcept {
        std::error_code ignored;
        (void)ignored;
#if defined(_WIN32)
        if (socket.is_open()) {
            (void)socket.release(ignored);
        }
#else
        if (descriptor.is_open()) {
            (void)descriptor.release();
        }
#endif
        native = static_cast<my_socket>(MARIADB_INVALID_SOCKET);
    }
};

}  // namespace detail

struct detail::MariaDbPool::OperationDeadline final {
    std::chrono::milliseconds fallbackTimeout{0};
    std::chrono::steady_clock::time_point deadline{};
    bool hasDeadline{false};

    explicit OperationDeadline(std::chrono::milliseconds timeout) noexcept
        : fallbackTimeout(timeout),
          deadline(std::chrono::steady_clock::now() + timeout),
          hasDeadline(timeout.count() > 0) {}

    [[nodiscard]] std::chrono::milliseconds remaining() const noexcept {
        if (!hasDeadline) {
            return fallbackTimeout;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    }
};

namespace {

void configureMariaDbTlsWorkaround() noexcept {
#if defined(_WIN32)
    (void)_putenv_s("MARIADB_TLS_DISABLE_PEER_VERIFICATION", "1");
#else
    (void)setenv("MARIADB_TLS_DISABLE_PEER_VERIFICATION", "1", 1);
#endif
}

class MysqlLibraryEnv final {
public:
    MysqlLibraryEnv() {
        configureMariaDbTlsWorkaround();
        (void)mysql_library_init(0, nullptr, nullptr);
    }

    ~MysqlLibraryEnv() {
        mysql_library_end();
    }
};

class MysqlThreadEnv final {
public:
    MysqlThreadEnv() {
        (void)mysql_thread_init();
    }

    ~MysqlThreadEnv() {
        mysql_thread_end();
    }
};

void ensureMysqlThreadInitialized() {
    static MysqlLibraryEnv libraryEnv;
    static thread_local MysqlThreadEnv threadEnv;
    (void)libraryEnv;
    (void)threadEnv;
}

std::runtime_error mysqlError(const st_mysql& connection, std::string_view operation) {
    auto* mutableConnection = const_cast<st_mysql*>(&connection);
    const auto* message = mysql_error(const_cast<st_mysql*>(&connection));
    const auto code = mysql_errno(mutableConnection);
    const auto* state = mysql_sqlstate(mutableConnection);
    std::pmr::string error(operation, std::pmr::get_default_resource());
    error.append(" failed");
    if (code != 0) {
        error.append(" [errno=");
        detail::appendDbNumber(error, static_cast<std::uint64_t>(code));
        error.push_back(']');
    }
    if (state != nullptr && state[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state);
        error.push_back(']');
    }
    if (message != nullptr && message[0] != '\0') {
        error.append(": ");
        error.append(message);
    }
    return std::runtime_error(error.c_str());
}

void appendStringLiteral(st_mysql& connection, std::pmr::string& output, std::string_view value) {
    output.push_back('\'');
    const auto offset = output.size();
    output.resize(output.size() + value.size() * 2 + 1);
    const auto length = mysql_real_escape_string(
        &connection,
        output.data() + offset,
        value.empty() ? "" : value.data(),
        static_cast<unsigned long>(value.size()));
    output.resize(offset + length);
    output.push_back('\'');
}

void appendValueLiteral(st_mysql& connection, std::pmr::string& output, const DbValue& value) {
    switch (value.type()) {
        case DbValueType::kNull:
            output.append("NULL");
            break;
        case DbValueType::kString:
            appendStringLiteral(connection, output, value.text());
            break;
        case DbValueType::kSigned:
            detail::appendDbNumber(output, value.signedValue());
            break;
        case DbValueType::kUnsigned:
            detail::appendDbNumber(output, value.unsignedValue());
            break;
        case DbValueType::kDouble:
            detail::appendDbNumber(output, value.doubleValue());
            break;
        case DbValueType::kBool:
            output.push_back(value.boolValue() ? '1' : '0');
            break;
    }
}

void freeStoredResult(st_mysql_res* result) noexcept {
    mysql_free_result(result);
}

[[nodiscard]] std::pmr::string interpolateSql(
    st_mysql& connection,
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(resource == nullptr ? std::pmr::get_default_resource() : resource);
    output.reserve(sql.size() + params.size() * 8);

    std::size_t offset = 0;
    for (const auto& param : params) {
        const auto placeholder = sql.find('?', offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        output.append(sql.data() + offset, placeholder - offset);
        appendValueLiteral(connection, output, param);
        offset = placeholder + 1;
    }

    if (sql.find('?', offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }

    output.append(sql.data() + offset, sql.size() - offset);
    return output;
}


[[nodiscard]] unsigned int timeoutSeconds(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }

    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        timeout + std::chrono::milliseconds(999));
    return static_cast<unsigned int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, seconds.count())),
        std::numeric_limits<unsigned int>::max()));
}

void setMysqlTimeout(st_mysql& connection, mysql_option option, std::chrono::milliseconds timeout) noexcept {
    const auto seconds = timeoutSeconds(timeout);
    if (seconds == 0) {
        return;
    }
    (void)mysql_options(&connection, option, &seconds);
}

void cancelSlotSocket(detail::SlotSocket& slotSocket) noexcept {
    std::error_code ignored;
#if defined(_WIN32)
    slotSocket.socket.cancel(ignored);
#else
    slotSocket.descriptor.cancel(ignored);
#endif
}

}  // namespace

detail::MariaDbPool::ConnectionSlot::ConnectionSlot(std::pmr::memory_resource*) noexcept {}

detail::MariaDbPool::ConnectionSlot::~ConnectionSlot() = default;
detail::MariaDbPool::ConnectionSlot::ConnectionSlot(ConnectionSlot&&) noexcept = default;
detail::MariaDbPool::ConnectionSlot& detail::MariaDbPool::ConnectionSlot::operator=(ConnectionSlot&&) noexcept = default;

detail::MariaDbPool::MariaDbPool(asio::io_context& ioContext, DbConfig config, std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      slots_(resource_),
      freeSlots_(resource_) {
    slots_.reserve(std::max<std::size_t>(1, config_.poolSize));
    freeSlots_.reserve(std::max<std::size_t>(1, config_.poolSize));
    for (std::size_t i = 0; i < std::max<std::size_t>(1, config_.poolSize); ++i) {
        slots_.emplace_back(resource_);
        freeSlots_.push_back(i);
    }
}

detail::MariaDbPool::~MariaDbPool() {
    closeNow();
}

Task<void> detail::MariaDbPool::connect() {
    for (auto& slot : slots_) {
        co_await connectUnlocked(slot);
    }
}

void detail::MariaDbPool::closeNow() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr) {
            *waiter->ready = true;
        }
        if (waiter->slot != nullptr) {
            *waiter->slot = slots_.size();
        }
        if (waiter->handle) {
            waiter->handle.resume();
        }
    }
    for (auto& slot : slots_) {
        closeSlot(slot);
    }
}

void detail::MariaDbPool::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    auto* waiter = waiterHead_;
    while (waiter != nullptr) {
        auto* next = waiter->next;
        if (config_.acquireTimeout.count() > 0 && waiter->deadline <= now) {
            removeWaiter(*waiter);
            if (waiter->timedOut != nullptr) {
                *waiter->timedOut = true;
            }
            if (waiter->ready != nullptr) {
                *waiter->ready = true;
            }
            if (waiter->handle) {
                waiter->handle.resume();
            }
        }
        waiter = next;
    }

    for (auto& slot : slots_) {
        if (!slot.deadlineActive || slot.deadline > now) {
            continue;
        }
        slot.timedOut = true;
        if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket) {
            if (slot.waitSocket != nullptr) {
                cancelSlotSocket(*slot.waitSocket);
            }
        } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
            auto handle = slot.deadlineContinuation;
            slot.deadlineContinuation = {};
            if (handle) {
                handle.resume();
            }
        }
    }
}

bool detail::MariaDbPool::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 ||
        config_.queryTimeout.count() > 0 ||
        config_.readTimeout.count() > 0 ||
        config_.writeTimeout.count() > 0 ||
        config_.acquireTimeout.count() > 0;
}

Task<QueryResult> detail::MariaDbPool::execute(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const auto slotIndex = co_await acquireSlot();
    SlotGuard guard(*this, slotIndex);
    try {
        co_return co_await executeOnSlot(
            slots_[slotIndex],
            std::string_view(sql.data(), sql.size()),
            std::span<const DbValue>(params.data(), params.size()),
            resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        throw;
    }
}

Task<DbStreamResult> detail::MariaDbPool::stream(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const auto slotIndex = co_await acquireSlot();
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }

        OperationDeadline deadline(config_.queryTimeout);
        std::pmr::string interpolatedSql(resource == nullptr ? std::pmr::get_default_resource() : resource);
        if (!params.empty()) {
            interpolatedSql = interpolateSql(
                *slot.connection,
                std::string_view(sql.data(), sql.size()),
                std::span<const DbValue>(params.data(), params.size()),
                resource);
            sql = std::move(interpolatedSql);
        }

        co_await runMysqlQuery(slot, std::string_view(sql.data(), sql.size()), deadline);
        auto* rawResult = mysql_use_result(slot.connection);
        if (rawResult == nullptr) {
            if (mysql_field_count(slot.connection) != 0) {
                throw mysqlError(*slot.connection, "mysql_use_result");
            }
            releaseSlot(slotIndex);
            co_return DbStreamResult(*this, slotIndex, nullptr, resource);
        }

        co_return DbStreamResult(*this, slotIndex, rawResult, resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }
}

Task<std::optional<DbRow>> detail::MariaDbPool::readStreamRow(
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return std::nullopt;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationDeadline deadline(config_.queryTimeout);
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, rawResult);
        while (status != 0) {
            status = mysql_fetch_row_cont(
                &row,
                rawResult,
                co_await waitForMysql(slots_[slot], status, deadline));
        }

        if (row == nullptr) {
            co_await closeStream(slot, result, resource);
            co_return std::nullopt;
        }

        const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
        const auto* lengths = mysql_fetch_lengths(rawResult);
        DbRow outputRow(resource);
        outputRow.reserve(fieldCount);
        for (std::size_t i = 0; i < fieldCount; ++i) {
            if (row[i] == nullptr) {
                outputRow.emplace_back(nullptr, resource);
                continue;
            }
            outputRow.emplace_back(std::string_view(row[i], lengths[i]), resource);
        }
        co_return outputRow;
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

Task<void> detail::MariaDbPool::closeStream(
    std::size_t slot,
    void* result,
    std::pmr::memory_resource*) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationDeadline deadline(config_.queryTimeout);
        int status = mysql_free_result_start(rawResult);
        while (status != 0) {
            status = mysql_free_result_cont(
                rawResult,
                co_await waitForMysql(slots_[slot], status, deadline));
        }
        releaseSlot(slot);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

void detail::MariaDbPool::abortStream(std::size_t slot, void*) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

Task<QueryResult> detail::MariaDbPool::executeOnTransactionSlot(
    std::size_t slot,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_return co_await executeOnSlot(
            slots_[slot],
            std::string_view(sql.data(), sql.size()),
            std::span<const DbValue>(params.data(), params.size()),
            resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

Task<QueryResult> detail::MariaDbPool::executeOnSlot(
    ConnectionSlot& slot,
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (!slot.connected) {
        co_await connectUnlocked(slot);
    }
    OperationDeadline deadline(config_.queryTimeout);

    std::pmr::string interpolatedSql(resource == nullptr ? std::pmr::get_default_resource() : resource);
    if (!params.empty()) {
        interpolatedSql = interpolateSql(
            *slot.connection,
            sql,
            params,
            resource);
        sql = std::string_view(interpolatedSql.data(), interpolatedSql.size());
    }

    auto& connection = *slot.connection;
    co_await runMysqlQuery(slot, sql, deadline);

    QueryResult result(resource);
    result.affectedRows_ = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    result.lastInsertId_ = static_cast<std::uint64_t>(mysql_insert_id(&connection));

    auto* rawResult = co_await storeMysqlResult(slot, deadline);
    if (rawResult == nullptr) {
        if (mysql_field_count(&connection) != 0) {
            throw mysqlError(connection, "mysql_store_result");
        }
        co_return result;
    }

    result.rawResult_ = rawResult;
    result.releaseRawResult_ = &freeStoredResult;
    const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
    const auto rowCount = static_cast<std::size_t>(mysql_num_rows(rawResult));
    result.rows_.reserve(rowCount);
    result.fields_.reserve(rowCount * fieldCount);
    while (auto* row = mysql_fetch_row(rawResult)) {
        const auto* lengths = mysql_fetch_lengths(rawResult);
        const auto rowStart = result.fields_.size();
        for (std::size_t i = 0; i < fieldCount; ++i) {
            if (row[i] == nullptr) {
                result.fields_.emplace_back(nullptr, resource);
                continue;
            }
            result.fields_.push_back(DbField::borrowed(std::string_view(row[i], lengths[i]), resource));
        }
        result.rows_.push_back(DbRow(result.fields_.data() + rowStart, fieldCount, resource));
    }

    co_return result;
}

Task<void> detail::MariaDbPool::executeControl(
    ConnectionSlot& slot,
    std::string_view sql,
    std::pmr::memory_resource* resource) {
    (void)co_await executeOnSlot(slot, sql, std::span<const DbValue>(), resource);
    co_return;
}

Task<DbTransaction> detail::MariaDbPool::beginTransaction(
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) {
    const auto slotIndex = co_await acquireSlot();
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }
        co_await executeControl(slot, "START TRANSACTION", resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }

    co_return DbTransaction(*this, slotIndex, resource, requestMemory);
}

Task<void> detail::MariaDbPool::commitTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_await executeControl(slots_[slot], "COMMIT", resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
    releaseSlot(slot);
}

Task<void> detail::MariaDbPool::rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_await executeControl(slots_[slot], "ROLLBACK", resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
    releaseSlot(slot);
}

void detail::MariaDbPool::abortTransaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

void detail::MariaDbPool::finishTransaction(std::size_t slot) noexcept {
    releaseSlot(slot);
}

detail::MariaDbPool::SlotGuard::SlotGuard(MariaDbPool& client, std::size_t slot) noexcept
    : client_(&client),
      slot_(slot) {}

detail::MariaDbPool::SlotGuard::~SlotGuard() {
    if (client_ != nullptr) {
        client_->releaseSlot(slot_);
    }
}

void detail::MariaDbPool::enqueueWaiter(SlotWaiter& waiter) noexcept {
    if (waiter.queued) {
        return;
    }
    waiter.previous = waiterTail_;
    waiter.next = nullptr;
    waiter.queued = true;
    if (waiterTail_ != nullptr) {
        waiterTail_->next = &waiter;
    } else {
        waiterHead_ = &waiter;
    }
    waiterTail_ = &waiter;
}

void detail::MariaDbPool::removeWaiter(SlotWaiter& waiter) noexcept {
    if (!waiter.queued) {
        return;
    }
    if (waiter.previous != nullptr) {
        waiter.previous->next = waiter.next;
    } else {
        waiterHead_ = waiter.next;
    }
    if (waiter.next != nullptr) {
        waiter.next->previous = waiter.previous;
    } else {
        waiterTail_ = waiter.previous;
    }
    waiter.previous = nullptr;
    waiter.next = nullptr;
    waiter.queued = false;
}

bool detail::MariaDbPool::resumeNextWaiter(std::size_t slot) noexcept {
    while (waiterHead_ != nullptr) {
        auto* waiter = waiterHead_;
        removeWaiter(*waiter);
        if (waiter->ready != nullptr && waiter->slot != nullptr) {
            *waiter->slot = slot;
            *waiter->ready = true;
            if (waiter->handle) {
                waiter->handle.resume();
            }
            return true;
        }
    }
    return false;
}

Task<std::size_t> detail::MariaDbPool::acquireSlot() {
    if (closing_) {
        throw std::runtime_error("database client is closing");
    }
    if (!freeSlots_.empty()) {
        const auto slot = freeSlots_.back();
        freeSlots_.pop_back();
        slots_[slot].busy = true;
        co_return slot;
    }

    struct WaiterGuard final {
        MariaDbPool& client;
        SlotWaiter& waiter;

        ~WaiterGuard() {
            client.removeWaiter(waiter);
        }
    };

    bool ready = false;
    bool timedOut = false;
    std::size_t slot = 0;
    SlotWaiter waiter{
        .ready = &ready,
        .timedOut = &timedOut,
        .slot = &slot,
        .deadline = std::chrono::steady_clock::now() + config_.acquireTimeout};
    enqueueWaiter(waiter);
    WaiterGuard guard{*this, waiter};

    struct WaiterAwaiter final {
        SlotWaiter& waiter;
        bool& ready;

        [[nodiscard]] bool await_ready() const noexcept {
            return ready;
        }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter.handle = handle;
        }

        void await_resume() const noexcept {}
    };

    co_await WaiterAwaiter{waiter, ready};

    if (timedOut) {
        throw std::runtime_error("database connection pool acquire timed out");
    }

    if (closing_ || slot >= slots_.size()) {
        throw std::runtime_error("database client is closing");
    }

    co_return slot;
}

void detail::MariaDbPool::releaseSlot(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    if (!closing_ && resumeNextWaiter(slot)) {
        return;
    }

    if (slots_[slot].busy) {
        slots_[slot].busy = false;
        freeSlots_.push_back(slot);
    }
}

void detail::MariaDbPool::closeSlot(ConnectionSlot& slot) noexcept {
    if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSocket && slot.waitSocket != nullptr) {
        cancelSlotSocket(*slot.waitSocket);
    } else if (slot.deadlineKind == ConnectionSlot::DeadlineKind::kSleep) {
        auto handle = slot.deadlineContinuation;
        slot.deadlineContinuation = {};
        if (handle) {
            handle.resume();
        }
    }
    clearSlotDeadline(slot);
    // Detach the fd from ASIO before mysql_close() closes it.
    if (slot.waitSocket != nullptr) {
        slot.waitSocket->release();
        slot.waitSocket.reset();
    }
    if (slot.connection != nullptr) {
        mysql_close(slot.connection);
        slot.connection = nullptr;
    }
    slot.connected = false;
}

void detail::MariaDbPool::setSlotDeadline(
    ConnectionSlot& slot,
    std::chrono::milliseconds timeout,
    ConnectionSlot::DeadlineKind kind) noexcept {
    slot.deadlineKind = kind;
    slot.timedOut = false;
    if (timeout.count() <= 0) {
        slot.deadlineActive = false;
        return;
    }
    slot.deadline = std::chrono::steady_clock::now() + timeout;
    slot.deadlineActive = true;
}

void detail::MariaDbPool::clearSlotDeadline(ConnectionSlot& slot) noexcept {
    slot.deadlineActive = false;
    slot.deadlineKind = ConnectionSlot::DeadlineKind::kNone;
    slot.deadlineContinuation = {};
}

Task<void> detail::MariaDbPool::connectUnlocked(ConnectionSlot& slot) {
    if (slot.connected) {
        co_return;
    }

    ensureMysqlThreadInitialized();
    if (slot.connection == nullptr) {
        auto* connection = mysql_init(nullptr);
        if (connection == nullptr) {
            throw std::runtime_error("mysql_init failed");
        }
        slot.connection = connection;
        slot.waitSocket = std::make_unique<detail::SlotSocket>(ioContext_);
        constexpr std::size_t kMysqlAsyncStackBytes = 1024 * 1024;
        (void)mysql_options(slot.connection, MYSQL_OPT_NONBLOCK, &kMysqlAsyncStackBytes);
        setMysqlTimeout(*slot.connection, MYSQL_OPT_CONNECT_TIMEOUT, config_.connectTimeout);
        setMysqlTimeout(*slot.connection, MYSQL_OPT_READ_TIMEOUT, config_.readTimeout);
        setMysqlTimeout(*slot.connection, MYSQL_OPT_WRITE_TIMEOUT, config_.writeTimeout);
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
    auto timeout = deadline.remaining();
    if (deadline.hasDeadline && timeout.count() <= 0) {
        co_return MYSQL_WAIT_TIMEOUT;
    }
    const auto wantsRead = (status & MYSQL_WAIT_READ) != 0;
    const auto wantsWrite = (status & MYSQL_WAIT_WRITE) != 0;
    const auto wantsException = (status & MYSQL_WAIT_EXCEPT) != 0;
    if (!wantsRead && !wantsWrite && !wantsException) {
        auto timeoutMs = timeout;
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

    auto timeoutMs = timeout;
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
                cancelSlotSocket(slotSocket);
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


detail::DbRegistry::DbRegistry(
    asio::io_context& ioContext,
    std::pmr::memory_resource* resource,
    std::span<const detail::DbDefinition> databases)
    : resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      clients_(resource_) {
    clients_.reserve(databases.size());
    for (const auto& definition : databases) {
        if (definition.alias.empty()) {
            throw std::invalid_argument("database alias must not be empty");
        }
        if (std::ranges::any_of(
                clients_,
                [&definition](const Entry& entry) {
                    return std::string_view(entry.alias.data(), entry.alias.size()) ==
                        std::string_view(definition.alias);
                })) {
            throw std::invalid_argument("duplicate database alias");
        }

        clients_.push_back(Entry{
            std::pmr::string(definition.alias, resource_),
            std::make_unique<MariaDbPool>(ioContext, definition.config, resource_)});
        if (std::string_view(clients_.back().alias.data(), clients_.back().alias.size()) == kDefaultDbAlias) {
            defaultClient_ = clients_.back().client.get();
        }
    }
}

detail::DbRegistry::~DbRegistry() = default;

Task<void> detail::DbRegistry::connect() {
    for (auto& entry : clients_) {
        co_await entry.client->connect();
    }
    co_return;
}

void detail::DbRegistry::closeNow() noexcept {
    for (auto& entry : clients_) {
        entry.client->closeNow();
    }
}

bool detail::DbRegistry::empty() const noexcept {
    return clients_.empty();
}

void detail::DbRegistry::scanDeadlines() noexcept {
    const auto now = std::chrono::steady_clock::now();
    for (auto& entry : clients_) {
        entry.client->scanDeadlines(now);
    }
}

bool detail::DbRegistry::hasAnyTimeout() const noexcept {
    return std::ranges::any_of(clients_, [](const Entry& entry) {
        return entry.client->hasAnyTimeout();
    });
}

DbHandle detail::DbRegistry::get(std::pmr::memory_resource* resource, RequestMemory* requestMemory) const {
    if (defaultClient_ == nullptr) {
        throw std::logic_error("default database is not configured");
    }
    return DbHandle(*defaultClient_, resource, requestMemory);
}

DbHandle detail::DbRegistry::get(
    std::string_view alias,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) const {
    for (const auto& entry : clients_) {
        if (std::string_view(entry.alias.data(), entry.alias.size()) == alias) {
            return DbHandle(*entry.client, resource, requestMemory);
        }
    }

    throw std::logic_error("database is not configured");
}

DbHandle Context::db() const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(resource(), const_cast<RequestMemory*>(&memory_));
}

DbHandle Context::db(std::string_view alias) const {
    if (db_ == nullptr) {
        throw std::logic_error("database is not configured");
    }
    return db_->get(alias, resource(), const_cast<RequestMemory*>(&memory_));
}

}  // namespace ruvia
