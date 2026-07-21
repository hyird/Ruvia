#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace ruvia {

class RequestMemory;
class DbValue;

enum class DbDriver : std::uint8_t {
    kMariaDb,
    kPostgreSql,
};

struct DbConfig {
    DbDriver driver{DbDriver::kMariaDb};
    // Host name or unbracketed address only; keep the port in port.
    std::pmr::string host{"127.0.0.1"};
    // Must be non-zero.
    std::uint16_t port{3306};
    std::pmr::string username;
    std::pmr::string password;
    std::pmr::string database;
    // Absence disables the corresponding timeout.
    std::optional<std::chrono::milliseconds> connectTimeout;
    std::optional<std::chrono::milliseconds> readTimeout;
    std::optional<std::chrono::milliseconds> writeTimeout;
    std::optional<std::chrono::milliseconds> queryTimeout;
    std::optional<std::chrono::milliseconds> acquireTimeout;

    [[nodiscard]] static DbConfig mariaDb() {
        return DbConfig{};
    }

    [[nodiscard]] static DbConfig postgreSql() {
        DbConfig config;
        config.driver = DbDriver::kPostgreSql;
        config.port = 5432;
        return config;
    }
};

namespace detail {

struct DbDefinition final {
    std::pmr::string alias;
    DbConfig config;
};

class MariaDbPool;
class PostgreSqlPool;
class DbRegistry;
class DbMigrationRunner;
struct DbValueAccess;
struct DbResultAccess;

enum class DbValueType : std::uint8_t {
    kNull,
    kString,
    kSigned,
    kUnsigned,
    kDouble,
    kBool
};

}  // namespace detail

class DbValue final {
private:
    struct BorrowedText final {
        std::string_view value;
    };

    using Storage = std::variant<
        std::monostate,
        BorrowedText,
        std::pmr::string,
        std::int64_t,
        std::uint64_t,
        double,
        bool>;

public:
    DbValue(std::nullptr_t);
    // Text is borrowed until a database operation synchronously clones the
    // parameter. The source must outlive this value; owning-string rvalues are
    // rejected so a stored DbValue cannot retain a destroyed temporary.
    DbValue(const char* value);
    DbValue(std::string_view value);

    template <typename Traits, typename Allocator>
    DbValue(std::basic_string<char, Traits, Allocator>&&) = delete;

    template <typename Traits, typename Allocator>
    DbValue(const std::basic_string<char, Traits, Allocator>&&) = delete;

    DbValue(bool value);

    DbValue(const DbValue&) = default;
    DbValue(DbValue&&) noexcept = default;
    DbValue& operator=(const DbValue&) = delete;
    DbValue& operator=(DbValue&&) = delete;

    template <typename T>
        requires (std::is_integral_v<std::remove_cvref_t<T>> &&
                  !std::is_same_v<std::remove_cvref_t<T>, bool>)
    DbValue(T value)
        : storage_(makeIntegerStorage(value)) {}

    template <typename T>
        requires std::is_floating_point_v<std::remove_cvref_t<T>>
    DbValue(T value)
        : storage_(std::in_place_type<double>, static_cast<double>(value)) {}

private:
    friend struct detail::DbValueAccess;

    explicit DbValue(std::pmr::string value);

    [[nodiscard]] detail::DbValueType type() const noexcept;
    [[nodiscard]] std::string_view text() const & noexcept;
    [[nodiscard]] std::string_view text() const && = delete;
    [[nodiscard]] std::int64_t signedValue() const noexcept;
    [[nodiscard]] std::uint64_t unsignedValue() const noexcept;
    [[nodiscard]] double doubleValue() const noexcept;
    [[nodiscard]] bool boolValue() const noexcept;

    template <typename T>
    [[nodiscard]] static Storage makeIntegerStorage(T value) {
        if constexpr (std::is_signed_v<std::remove_cvref_t<T>>) {
            return Storage(
                std::in_place_type<std::int64_t>,
                static_cast<std::int64_t>(value));
        } else {
            return Storage(
                std::in_place_type<std::uint64_t>,
                static_cast<std::uint64_t>(value));
        }
    }

    Storage storage_;
};

class DbField final {
private:
    struct BorrowedText final {
        std::string_view value;
    };

    using Storage = std::variant<
        std::monostate,
        std::pmr::string,
        BorrowedText>;

public:
    DbField(DbField&& other) noexcept;
    DbField& operator=(DbField&& other);

    DbField(const DbField&) = delete;
    DbField& operator=(const DbField&) = delete;

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] std::string_view text() const & noexcept;
    [[nodiscard]] std::string_view text() const && = delete;

private:
    friend struct detail::DbResultAccess;

    struct BorrowedTag final {};

    explicit DbField(std::pmr::memory_resource* resource);
    DbField(std::nullptr_t, std::pmr::memory_resource* resource);
    DbField(std::string_view value, std::pmr::memory_resource* resource);
    DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static DbField borrowed(std::string_view value, std::pmr::memory_resource* resource);

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

class DbRow final {
private:
    using OwnedFields = std::pmr::vector<DbField>;
    using BorrowedFields = std::span<const DbField>;
    using Storage = std::variant<OwnedFields, BorrowedFields>;

public:
    DbRow(DbRow&& other) noexcept;
    DbRow& operator=(DbRow&& other);

    DbRow(const DbRow&) = delete;
    DbRow& operator=(const DbRow&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DbField& operator[](
        std::size_t index) const & noexcept;
    [[nodiscard]] const DbField& operator[](
        std::size_t index) const && = delete;
    [[nodiscard]] const DbField* begin() const & noexcept;
    [[nodiscard]] const DbField* begin() const && = delete;
    [[nodiscard]] const DbField* end() const & noexcept;
    [[nodiscard]] const DbField* end() const && = delete;

private:
    friend struct detail::DbResultAccess;

    explicit DbRow(std::pmr::memory_resource* resource = nullptr);
    DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource);
    [[nodiscard]] OwnedFields& ownedFields() noexcept;

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

}  // namespace ruvia
