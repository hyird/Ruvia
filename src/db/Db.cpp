#include "DbInternal.h"
#include "ruvia/http/Context.h"

#include <asio/co_spawn.hpp>
#ifdef _WIN32
#include <winsock2.h>
#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif
#include <asio/use_future.hpp>
#include <mysql/mysql.h>

#include "../AsioAwait.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <type_traits>
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

void appendDiagnosticNumber(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format database diagnostic code");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
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
        appendDiagnosticNumber(error, static_cast<std::uint64_t>(code));
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

void appendNumberLiteral(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format signed database value");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void appendNumberLiteral(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format unsigned database value");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

void appendNumberLiteral(std::pmr::string& output, double value) {
    std::array<char, 64> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument("database double value cannot be formatted");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
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
            appendNumberLiteral(output, value.signedValue());
            break;
        case DbValueType::kUnsigned:
            appendNumberLiteral(output, value.unsignedValue());
            break;
        case DbValueType::kDouble:
            appendNumberLiteral(output, value.doubleValue());
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

DbValue cloneDbValueForResource(const DbValue& value, std::pmr::memory_resource* resource) {
    switch (value.type()) {
        case DbValueType::kNull:
            return DbValue(nullptr);
        case DbValueType::kString:
            return DbValue(std::pmr::string(value.text(), resource));
        case DbValueType::kSigned:
            return DbValue(value.signedValue());
        case DbValueType::kUnsigned:
            return DbValue(value.unsignedValue());
        case DbValueType::kDouble:
            return DbValue(value.doubleValue());
        case DbValueType::kBool:
            return DbValue(value.boolValue());
    }
    return DbValue(nullptr);
}

[[nodiscard]] std::pmr::memory_resource* resolveResource(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

[[nodiscard]] std::pmr::vector<DbValue> cloneDbValues(
    std::span<const DbValue> values,
    std::pmr::memory_resource* resource) {
    auto* resolved = resolveResource(resource);
    std::pmr::vector<DbValue> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(cloneDbValueForResource(value, resolved));
    }
    return output;
}

[[nodiscard]] bool isValidMigrationTableName(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const auto ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

void appendQuotedIdentifier(std::pmr::string& sql, std::string_view identifier) {
    if (!isValidMigrationTableName(identifier)) {
        throw std::invalid_argument("database migration table must contain only letters, digits and underscores");
    }
    sql.push_back('`');
    sql.append(identifier);
    sql.push_back('`');
}

void validateMigrationList(std::span<const DbMigration> migrations) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const auto& migration = migrations[i];
        if (migration.id.empty()) {
            throw std::invalid_argument("database migration id must not be empty");
        }
        if (migration.id.size() > 190) {
            throw std::invalid_argument("database migration id must not exceed 190 bytes");
        }
        if (migration.sql.empty()) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migrations[j].id == migration.id) {
                throw std::invalid_argument("database migration ids must be unique");
            }
        }
    }
}

[[nodiscard]] std::pmr::string buildMigrationLockName(
    const DbConfig& config,
    std::pmr::memory_resource* resource) {
    std::pmr::string name(resource);
    name.append("ruvia:migrations:");
    if (!config.database.empty()) {
        name.append(config.database);
    } else {
        name.append(config.host);
        name.push_back(':');
        appendNumberLiteral(name, static_cast<std::uint64_t>(config.port));
    }
    return name;
}

[[nodiscard]] std::pmr::string buildCreateMigrationsTableSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.append("CREATE TABLE IF NOT EXISTS ");
    appendQuotedIdentifier(sql, table);
    sql.append(
        " (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " migration_id VARCHAR(190) NOT NULL,"
        " applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " UNIQUE KEY `uk_migration_id` (`migration_id`))"
        " ENGINE=InnoDB");
    return sql;
}

[[nodiscard]] std::pmr::string buildFindMigrationSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.append("SELECT migration_id FROM ");
    appendQuotedIdentifier(sql, table);
    sql.append(" WHERE migration_id = ? LIMIT 1");
    return sql;
}

[[nodiscard]] std::pmr::string buildInsertMigrationSql(
    std::string_view table,
    std::pmr::memory_resource* resource) {
    std::pmr::string sql(resource);
    sql.append("INSERT INTO ");
    appendQuotedIdentifier(sql, table);
    sql.append(" (migration_id) VALUES (?)");
    return sql;
}

void appendMigrationId(std::pmr::vector<std::pmr::string>& ids, std::string_view id) {
    std::pmr::string stored(ids.get_allocator().resource());
    stored.assign(id.data(), id.size());
    ids.push_back(std::move(stored));
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

namespace {

[[maybe_unused]] Task<QueryResult> disabledQueryResult(std::pmr::memory_resource* resource) {
    throw std::logic_error("database support is disabled in this Ruvia build");
    co_return QueryResult(resource == nullptr ? std::pmr::get_default_resource() : resource);
}

}  // namespace

DbValue::DbValue(std::nullptr_t) {}

DbValue::DbValue(const char* value) {
    if (value == nullptr) {
        return;
    }

    type_ = DbValueType::kString;
    text_ = value;
}

DbValue::DbValue(std::string_view value) : type_(DbValueType::kString), text_(value) {}

DbValue::DbValue(std::pmr::string value)
    : type_(DbValueType::kString),
      ownedText_(std::move(value)),
      ownsText_(true) {}

DbValue::DbValue(bool value) : type_(DbValueType::kBool), boolValue_(value) {}

DbValueType DbValue::type() const noexcept {
    return type_;
}

std::string_view DbValue::text() const noexcept {
    if (ownsText_) {
        return ownedText_;
    }
    return text_;
}

std::int64_t DbValue::signedValue() const noexcept {
    return signedValue_;
}

std::uint64_t DbValue::unsignedValue() const noexcept {
    return unsignedValue_;
}

double DbValue::doubleValue() const noexcept {
    return doubleValue_;
}

bool DbValue::boolValue() const noexcept {
    return boolValue_;
}

DbField::DbField(std::pmr::memory_resource* resource)
    : value_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

DbField::DbField(std::nullptr_t, std::pmr::memory_resource* resource)
    : DbField(resource) {}

DbField::DbField(std::string_view value, std::pmr::memory_resource* resource)
    : isNull_(false),
      value_(value, resource == nullptr ? std::pmr::get_default_resource() : resource),
      valueView_(value_),
      ownsValue_(true) {}

DbField::DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource)
    : isNull_(false),
      value_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      valueView_(value),
      ownsValue_(false) {}

DbField DbField::borrowed(std::string_view value, std::pmr::memory_resource* resource) {
    return DbField(BorrowedTag{}, value, resource);
}

DbField::DbField(DbField&& other) noexcept
    : isNull_(std::exchange(other.isNull_, true)),
      value_(std::move(other.value_)),
      valueView_(std::exchange(other.valueView_, {})),
      ownsValue_(std::exchange(other.ownsValue_, false)) {
    refreshView();
}

DbField& DbField::operator=(DbField&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    isNull_ = std::exchange(other.isNull_, true);
    value_ = std::move(other.value_);
    valueView_ = std::exchange(other.valueView_, {});
    ownsValue_ = std::exchange(other.ownsValue_, false);
    refreshView();
    return *this;
}

bool DbField::isNull() const noexcept {
    return isNull_;
}

std::string_view DbField::text() const noexcept {
    return valueView_;
}

void DbField::refreshView() noexcept {
    if (ownsValue_) {
        valueView_ = value_;
    }
}

DbRow::DbRow(std::pmr::memory_resource* resource)
    : ownedFields_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

DbRow::DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource)
    : ownedFields_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      fields_(fields),
      size_(size),
      ownsFields_(false) {}

DbRow::DbRow(DbRow&& other) noexcept
    : ownedFields_(std::move(other.ownedFields_)),
      fields_(std::exchange(other.fields_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      ownsFields_(std::exchange(other.ownsFields_, true)) {
    refreshView();
}

DbRow& DbRow::operator=(DbRow&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    ownedFields_ = std::move(other.ownedFields_);
    fields_ = std::exchange(other.fields_, nullptr);
    size_ = std::exchange(other.size_, 0);
    ownsFields_ = std::exchange(other.ownsFields_, true);
    refreshView();
    return *this;
}

bool DbRow::empty() const noexcept {
    return size_ == 0;
}

std::size_t DbRow::size() const noexcept {
    return size_;
}

const DbField& DbRow::operator[](std::size_t index) const noexcept {
    return fields_[index];
}

const DbField* DbRow::begin() const noexcept {
    return fields_;
}

const DbField* DbRow::end() const noexcept {
    return fields_ + size_;
}

void DbRow::reserve(std::size_t size) {
    ownedFields_.reserve(size);
    refreshView();
}

void DbRow::push_back(DbField field) {
    ownedFields_.push_back(std::move(field));
    refreshView();
}

void DbRow::emplace_back(std::nullptr_t, std::pmr::memory_resource* resource) {
    ownedFields_.emplace_back(nullptr, resource);
    refreshView();
}

void DbRow::emplace_back(std::string_view value, std::pmr::memory_resource* resource) {
    ownedFields_.emplace_back(value, resource);
    refreshView();
}

void DbRow::refreshView() noexcept {
    if (ownsFields_) {
        fields_ = ownedFields_.data();
        size_ = ownedFields_.size();
    }
}

QueryResult::QueryResult(std::pmr::memory_resource* resource)
    : rows_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      fields_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

QueryResult::QueryResult(QueryResult&& other) noexcept
    : rows_(std::move(other.rows_)),
      fields_(std::move(other.fields_)),
      affectedRows_(std::exchange(other.affectedRows_, 0)),
      lastInsertId_(std::exchange(other.lastInsertId_, 0)),
      mounted_(std::exchange(other.mounted_, nullptr)),
      rawResult_(std::exchange(other.rawResult_, nullptr)),
      releaseRawResult_(std::exchange(other.releaseRawResult_, nullptr)) {}

QueryResult::~QueryResult() {
    if (rawResult_ != nullptr && releaseRawResult_ != nullptr) {
        releaseRawResult_(rawResult_);
    }
}

std::span<const DbRow> QueryResult::rows() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return std::span<const DbRow>(result.rows_.data(), result.rows_.size());
}

std::uint64_t QueryResult::affectedRows() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return result.affectedRows_;
}

std::uint64_t QueryResult::lastInsertId() const noexcept {
    const auto& result = mounted_ == nullptr ? *this : *mounted_;
    return result.lastInsertId_;
}

DbMigrationReport::DbMigrationReport(std::pmr::memory_resource* resource)
    : applied_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      skipped_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

std::span<const std::pmr::string> DbMigrationReport::applied() const noexcept {
    return std::span<const std::pmr::string>(applied_.data(), applied_.size());
}

std::span<const std::pmr::string> DbMigrationReport::skipped() const noexcept {
    return std::span<const std::pmr::string>(skipped_.data(), skipped_.size());
}

bool DbMigrationReport::changed() const noexcept {
    return !applied_.empty();
}

DbHandle::DbHandle(
    detail::MariaDbPool& client,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(client),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      requestMemory_(requestMemory) {}

QueryResult DbHandle::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }
    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

Task<QueryResult> DbHandle::query(std::string_view sql, std::initializer_list<DbValue> params) const {
    return query(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = cloneDbValues(params, resource_);

    auto result = co_await client_.execute(std::move(sqlCopy), std::move(paramCopy), resource_);
    co_return mountResult(std::move(result));
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::initializer_list<DbValue> params) const {
    return execute(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = cloneDbValues(params, resource_);

    auto result = co_await client_.execute(std::move(sqlCopy), std::move(paramCopy), resource_);
    co_return mountResult(std::move(result));
}

Task<DbStreamResult> DbHandle::queryStream(std::string_view sql, std::initializer_list<DbValue> params) const {
    return queryStream(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<DbStreamResult> DbHandle::queryStream(std::string_view sql, std::span<const DbValue> params) const {
    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = cloneDbValues(params, resource_);

    return client_.stream(std::move(sqlCopy), std::move(paramCopy), resource_);
}

Task<DbTransaction> DbHandle::beginTransaction() const {
    return client_.beginTransaction(resource_, requestMemory_);
}

DbStreamResult::DbStreamResult(
    detail::MariaDbPool& client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : client_(&client),
      slot_(slot),
      result_(result),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      active_(result != nullptr) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : client_(std::exchange(other.client_, nullptr)),
      slot_(std::exchange(other.slot_, 0)),
      result_(std::exchange(other.result_, nullptr)),
      resource_(std::exchange(other.resource_, std::pmr::get_default_resource())),
      active_(std::exchange(other.active_, false)) {}

DbStreamResult& DbStreamResult::operator=(DbStreamResult&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, nullptr);
    slot_ = std::exchange(other.slot_, 0);
    result_ = std::exchange(other.result_, nullptr);
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return active_;
}

Task<std::optional<DbRow>> DbStreamResult::read() {
    if (!active_ || client_ == nullptr || result_ == nullptr) {
        co_return std::nullopt;
    }

    try {
        auto row = co_await client_->readStreamRow(slot_, result_, resource_);
        if (!row) {
            release();
        }
        co_return row;
    } catch (...) {
        release();
        throw;
    }
}

Task<void> DbStreamResult::close() {
    if (!active_ || client_ == nullptr || result_ == nullptr) {
        co_return;
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* result = result_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    co_await client->closeStream(slot, result, resource);
}

void DbStreamResult::reset() noexcept {
    if (active_ && client_ != nullptr && result_ != nullptr) {
        client_->abortStream(slot_, result_);
    }
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

void DbStreamResult::release() noexcept {
    client_ = nullptr;
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

DbTransaction::DbTransaction(
    detail::MariaDbPool& client,
    std::size_t slot,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(&client),
      slot_(slot),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      requestMemory_(requestMemory),
      active_(true) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : client_(std::exchange(other.client_, nullptr)),
      slot_(std::exchange(other.slot_, 0)),
      resource_(std::exchange(other.resource_, std::pmr::get_default_resource())),
      requestMemory_(std::exchange(other.requestMemory_, nullptr)),
      active_(std::exchange(other.active_, false)) {}

DbTransaction& DbTransaction::operator=(DbTransaction&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, nullptr);
    slot_ = std::exchange(other.slot_, 0);
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
    requestMemory_ = std::exchange(other.requestMemory_, nullptr);
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return active_;
}

QueryResult DbTransaction::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }

    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

Task<QueryResult> DbTransaction::query(std::string_view sql, std::initializer_list<DbValue> params) {
    return query(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbTransaction::query(std::string_view sql, std::span<const DbValue> params) {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }

    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = cloneDbValues(params, resource_);

    try {
        auto result = co_await client_->executeOnTransactionSlot(
            slot_,
            std::move(sqlCopy),
            std::move(paramCopy),
            resource_);
        co_return mountResult(std::move(result));
    } catch (...) {
        client_ = nullptr;
        slot_ = 0;
        active_ = false;
        throw;
    }
}

Task<QueryResult> DbTransaction::execute(std::string_view sql, std::initializer_list<DbValue> params) {
    return execute(sql, std::span<const DbValue>(params.begin(), params.size()));
}

Task<QueryResult> DbTransaction::execute(std::string_view sql, std::span<const DbValue> params) {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }

    std::pmr::string sqlCopy(sql, resource_);
    auto paramCopy = cloneDbValues(params, resource_);

    try {
        auto result = co_await client_->executeOnTransactionSlot(
            slot_,
            std::move(sqlCopy),
            std::move(paramCopy),
            resource_);
        co_return mountResult(std::move(result));
    } catch (...) {
        client_ = nullptr;
        slot_ = 0;
        active_ = false;
        throw;
    }
}

Task<void> DbTransaction::commit() {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    return client->commitTransaction(slot, resource);
}

Task<void> DbTransaction::rollback() {
    if (!active_ || client_ == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto* client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = nullptr;
    slot_ = 0;
    return client->rollbackTransaction(slot, resource);
}

void DbTransaction::reset() noexcept {
    if (active_ && client_ != nullptr) {
        client_->abortTransaction(slot_);
    }
    client_ = nullptr;
    slot_ = 0;
    active_ = false;
}

void DbTransaction::release() noexcept {
    if (active_ && client_ != nullptr) {
        client_->finishTransaction(slot_);
    }
    client_ = nullptr;
    slot_ = 0;
    active_ = false;
}

class detail::DbMigrationRunner final {
public:
    [[nodiscard]] static Task<DbMigrationReport> run(
        asio::io_context& ioContext,
        DbConfig config,
        std::span<const DbMigration> migrations,
        DbMigrationOptions options,
        std::pmr::memory_resource* resource) {
        auto* resolved = resolveResource(resource);
        validateMigrationList(migrations);
        if (!isValidMigrationTableName(options.table)) {
            throw std::invalid_argument("database migration table must contain only letters, digits and underscores");
        }

        config.poolSize = 1;
        if (config.acquireTimeout.count() == 0) {
            config.acquireTimeout = config.queryTimeout;
        }

        auto lockName = buildMigrationLockName(config, resolved);
        const detail::DbDefinition databases[] = {
            detail::DbDefinition{
                std::pmr::string(
                    detail::kDefaultDbAlias.data(),
                    detail::kDefaultDbAlias.size(),
                    resolved),
                std::move(config)}
        };
        detail::DbRegistry registry(ioContext, resolved, databases);
        co_await registry.connect();
        auto handle = registry.get(resolved);

        const auto lockSeconds = static_cast<std::int64_t>(options.lockTimeout.count());
        auto lockResult = co_await handle.query("SELECT GET_LOCK(?, ?)", {std::string_view(lockName), lockSeconds});
        if (lockResult.rows().size() != 1 ||
            lockResult.rows()[0].empty() ||
            lockResult.rows()[0][0].text() != "1") {
            registry.closeNow();
            throw std::runtime_error("database migration lock could not be acquired");
        }

        DbMigrationReport report(resolved);
        std::exception_ptr failure;
        try {
            (void)co_await handle.execute(buildCreateMigrationsTableSql(options.table, resolved));

            auto findSql = buildFindMigrationSql(options.table, resolved);
            auto insertSql = buildInsertMigrationSql(options.table, resolved);
            for (const auto& migration : migrations) {
                auto existing = co_await handle.query(findSql, {migration.id});
                if (!existing.rows().empty()) {
                    appendMigrationId(report.skipped_, migration.id);
                    continue;
                }

                (void)co_await handle.execute(migration.sql);
                (void)co_await handle.execute(insertSql, {migration.id});
                appendMigrationId(report.applied_, migration.id);
            }
        } catch (...) {
            failure = std::current_exception();
        }

        try {
            (void)co_await handle.execute("DO RELEASE_LOCK(?)", {std::string_view(lockName)});
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }

        registry.closeNow();
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        co_return report;
    }
};

DbMigrator::DbMigrator(
    DbConfig config,
    DbMigrationOptions options,
    std::pmr::memory_resource* resource)
    : config_(std::move(config)),
      options_(std::move(options)),
      resource_(resource == nullptr ? std::pmr::get_default_resource() : resource) {}

DbMigrationReport DbMigrator::migrate(std::span<const DbMigration> migrations) const {
    return migrate(config_, migrations, options_, resource_);
}

DbMigrationReport DbMigrator::migrate(
    DbConfig config,
    std::span<const DbMigration> migrations,
    DbMigrationOptions options,
    std::pmr::memory_resource* resource) {
    asio::io_context ioContext(1);
    auto future = asio::co_spawn(
        ioContext,
        detail::taskAsAwaitable(detail::DbMigrationRunner::run(
            ioContext,
            std::move(config),
            migrations,
            std::move(options),
            resource)),
        asio::use_future);
    ioContext.run();
    return future.get();
}


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
    (void)co_await executeOnSlot(slot, sql, {}, resource);
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
