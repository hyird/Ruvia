#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ruvia/app/Task.h"

struct st_mysql_res;

namespace ruvia {

class RequestMemory;

struct DbConfig {
    std::pmr::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::pmr::string username;
    std::pmr::string password;
    // MariaDB schema/database name.
    std::pmr::string database;
    // Fast-only mode: this is the number of connections owned by each worker.
    // Total connections are roughly configured worker count * poolSize per alias.
    std::size_t poolSize{4};
    std::chrono::milliseconds connectTimeout{0};
    std::chrono::milliseconds readTimeout{0};
    std::chrono::milliseconds writeTimeout{0};
    std::chrono::milliseconds queryTimeout{0};
    std::chrono::milliseconds acquireTimeout{0};
};

namespace detail {

struct DbDefinition final {
    std::pmr::string alias;
    DbConfig config;
};

class MariaDbPool;
class DbRegistry;
class DbMigrationRunner;

}  // namespace detail

enum class DbValueType {
    kNull,
    kString,
    kSigned,
    kUnsigned,
    kDouble,
    kBool
};

class DbValue final {
public:
    DbValue(std::nullptr_t);
    DbValue(const char* value);
    DbValue(std::string_view value);
    DbValue(std::pmr::string value);
    DbValue(bool value);

    template <typename T>
        requires (std::is_integral_v<std::remove_cvref_t<T>> &&
                  !std::is_same_v<std::remove_cvref_t<T>, bool>)
    DbValue(T value) {
        if constexpr (std::is_signed_v<std::remove_cvref_t<T>>) {
            type_ = DbValueType::kSigned;
            signedValue_ = static_cast<std::int64_t>(value);
        } else {
            type_ = DbValueType::kUnsigned;
            unsignedValue_ = static_cast<std::uint64_t>(value);
        }
    }

    template <typename T>
        requires std::is_floating_point_v<std::remove_cvref_t<T>>
    DbValue(T value) : type_(DbValueType::kDouble), doubleValue_(static_cast<double>(value)) {}

    [[nodiscard]] DbValueType type() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] std::int64_t signedValue() const noexcept;
    [[nodiscard]] std::uint64_t unsignedValue() const noexcept;
    [[nodiscard]] double doubleValue() const noexcept;
    [[nodiscard]] bool boolValue() const noexcept;

private:
    DbValueType type_{DbValueType::kNull};
    std::pmr::string ownedText_;
    std::string_view text_;
    bool ownsText_{false};
    std::int64_t signedValue_{0};
    std::uint64_t unsignedValue_{0};
    double doubleValue_{0.0};
    bool boolValue_{false};
};

class DbField final {
public:
    explicit DbField(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    DbField(std::nullptr_t, std::pmr::memory_resource* resource);
    DbField(std::string_view value, std::pmr::memory_resource* resource);
    DbField(DbField&& other) noexcept;
    DbField& operator=(DbField&& other) noexcept;

    DbField(const DbField&) = delete;
    DbField& operator=(const DbField&) = delete;

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;

private:
    friend class detail::MariaDbPool;

    struct BorrowedTag final {};

    DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static DbField borrowed(std::string_view value, std::pmr::memory_resource* resource);
    void refreshView() noexcept;

    bool isNull_{true};
    std::pmr::string value_;
    std::string_view valueView_;
    bool ownsValue_{false};
};

class DbRow final {
public:
    explicit DbRow(std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    DbRow(DbRow&& other) noexcept;
    DbRow& operator=(DbRow&& other) noexcept;

    DbRow(const DbRow&) = delete;
    DbRow& operator=(const DbRow&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DbField& operator[](std::size_t index) const noexcept;
    [[nodiscard]] const DbField* begin() const noexcept;
    [[nodiscard]] const DbField* end() const noexcept;

    void reserve(std::size_t size);
    void push_back(DbField field);
    void emplace_back(std::nullptr_t, std::pmr::memory_resource* resource);
    void emplace_back(std::string_view value, std::pmr::memory_resource* resource);

private:
    friend class detail::MariaDbPool;

    DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource);
    void refreshView() noexcept;

    std::pmr::vector<DbField> ownedFields_;
    const DbField* fields_{nullptr};
    std::size_t size_{0};
    bool ownsFields_{true};
};

class DbTransaction;
class DbStreamResult;
class DbMigrator;

struct DbMigration final {
    std::string_view id;
    std::string_view sql;
};

struct DbMigrationOptions final {
    std::pmr::string table{"ruvia_schema_migrations"};
    std::chrono::seconds lockTimeout{30};
};

class QueryResult final {
public:
    explicit QueryResult(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;
    QueryResult(QueryResult&& other) noexcept;
    QueryResult& operator=(QueryResult&& other) = delete;
    ~QueryResult();

    [[nodiscard]] std::span<const DbRow> rows() const noexcept;
    [[nodiscard]] std::uint64_t affectedRows() const noexcept;
    [[nodiscard]] std::uint64_t lastInsertId() const noexcept;

private:
    friend class detail::MariaDbPool;
    friend class DbHandle;
    friend class DbTransaction;

    std::pmr::vector<DbRow> rows_;
    std::pmr::vector<DbField> fields_;
    std::uint64_t affectedRows_{0};
    std::uint64_t lastInsertId_{0};
    const QueryResult* mounted_{nullptr};
    st_mysql_res* rawResult_{nullptr};
    void (*releaseRawResult_)(st_mysql_res*) noexcept{nullptr};
};

class DbMigrationReport final {
public:
    explicit DbMigrationReport(std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    DbMigrationReport(const DbMigrationReport&) = delete;
    DbMigrationReport& operator=(const DbMigrationReport&) = delete;
    DbMigrationReport(DbMigrationReport&&) noexcept = default;
    DbMigrationReport& operator=(DbMigrationReport&&) noexcept = default;

    [[nodiscard]] std::span<const std::pmr::string> applied() const noexcept;
    [[nodiscard]] std::span<const std::pmr::string> skipped() const noexcept;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class DbMigrator;
    friend class detail::DbMigrationRunner;

    std::pmr::vector<std::pmr::string> applied_;
    std::pmr::vector<std::pmr::string> skipped_;
};

class DbHandle final {
public:
    DbHandle(const DbHandle&) = default;
    DbHandle& operator=(const DbHandle&) = delete;

    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params) const;
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params) const;
    Task<DbStreamResult> queryStream(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<DbStreamResult> queryStream(std::string_view sql, std::span<const DbValue> params) const;
    Task<DbTransaction> beginTransaction() const;

private:
    friend class detail::DbRegistry;

    DbHandle(
        detail::MariaDbPool& client,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) noexcept;
    [[nodiscard]] QueryResult mountResult(QueryResult result) const;

    detail::MariaDbPool& client_;
    std::pmr::memory_resource* resource_;
    RequestMemory* requestMemory_{nullptr};
};

class DbStreamResult final {
public:
    DbStreamResult(const DbStreamResult&) = delete;
    DbStreamResult& operator=(const DbStreamResult&) = delete;
    DbStreamResult(DbStreamResult&& other) noexcept;
    DbStreamResult& operator=(DbStreamResult&& other) noexcept;
    ~DbStreamResult();

    [[nodiscard]] bool active() const noexcept;
    Task<std::optional<DbRow>> read();
    Task<void> close();

private:
    friend class detail::MariaDbPool;

    DbStreamResult(
        detail::MariaDbPool& client,
        std::size_t slot,
        void* result,
        std::pmr::memory_resource* resource) noexcept;
    void reset() noexcept;
    void release() noexcept;

    detail::MariaDbPool* client_{nullptr};
    std::size_t slot_{0};
    void* result_{nullptr};
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    bool active_{false};
};

class DbTransaction final {
public:
    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&& other) noexcept;
    DbTransaction& operator=(DbTransaction&& other) noexcept;
    ~DbTransaction();

    [[nodiscard]] bool active() const noexcept;
    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params = {});
    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params);
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params = {});
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params);
    Task<void> commit();
    Task<void> rollback();

private:
    friend class detail::MariaDbPool;

    DbTransaction(
        detail::MariaDbPool& client,
        std::size_t slot,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) noexcept;
    [[nodiscard]] QueryResult mountResult(QueryResult result) const;
    void reset() noexcept;
    void release() noexcept;

    detail::MariaDbPool* client_{nullptr};
    std::size_t slot_{0};
    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};
    RequestMemory* requestMemory_{nullptr};
    bool active_{false};
};

class DbMigrator final {
public:
    explicit DbMigrator(
        DbConfig config,
        DbMigrationOptions options = {},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

    [[nodiscard]] DbMigrationReport migrate(std::span<const DbMigration> migrations) const;

    [[nodiscard]] static DbMigrationReport migrate(
        DbConfig config,
        std::span<const DbMigration> migrations,
        DbMigrationOptions options = {},
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());

private:
    DbConfig config_;
    DbMigrationOptions options_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
