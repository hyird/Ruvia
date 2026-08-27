#pragma once

#include "ruvia/http/BorrowedText.h"
#include "ruvia/core/OperationOptions.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>
#include <vector>
#include <utility>

namespace ruvia {

class RequestMemory;
class DbValue;

enum class DbDriver : std::uint8_t {
    kUnspecified,
    kMariaDb,
    kPostgreSql,
};

struct DbConfig final {
    DbDriver driver{DbDriver::kUnspecified};
    // Host name or unbracketed address only; keep the port in port.
    std::string host{"127.0.0.1"};
    // Absence selects the driver's standard port: 3306 or 5432.
    std::optional<std::uint16_t> port{};
    std::string username{};
    std::string password{};
    std::string database{};
    // Absence disables the corresponding timeout.
    std::optional<std::chrono::milliseconds> connectTimeout{std::chrono::seconds(5)};
    std::optional<std::chrono::milliseconds> readTimeout{};
    std::optional<std::chrono::milliseconds> writeTimeout{};
    std::optional<std::chrono::milliseconds> queryTimeout{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> acquireTimeout{std::chrono::seconds(5)};
};

class DbError final : public std::runtime_error {
public:
    enum class Code : std::uint8_t {
        kNotConfigured,
        kResolveFailed,
        kConnectFailed,
        kIoError,
        kStatementFailed,
        kProtocolError,
        kTimeout,
        kCancelled,
        kClosing,
    };

    DbError(Code code, std::optional<DbDriver> driver, std::string message,
        std::optional<std::int64_t> nativeCode = std::nullopt, std::string sqlState = {},
        std::string constraintName = {})
        : std::runtime_error(std::move(message)),
          code_(code),
          driver_(driver),
          nativeCode_(nativeCode),
          sqlState_(std::move(sqlState)),
          constraintName_(std::move(constraintName)) {}

    [[nodiscard]] Code code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::optional<DbDriver> driver() const noexcept {
        return driver_;
    }

    [[nodiscard]] std::optional<std::int64_t> nativeCode() const noexcept {
        return nativeCode_;
    }

    [[nodiscard]] std::optional<std::string_view> sqlState() const& noexcept {
        if (sqlState_.empty()) {
            return std::nullopt;
        }
        return sqlState_;
    }

    [[nodiscard]] std::optional<std::string_view> sqlState() const&& = delete;

    [[nodiscard]] std::optional<std::string_view> constraintName() const& noexcept {
        if (constraintName_.empty()) {
            return std::nullopt;
        }
        return constraintName_;
    }

    [[nodiscard]] std::optional<std::string_view> constraintName() const&& = delete;

private:
    Code code_;
    std::optional<DbDriver> driver_;
    std::optional<std::int64_t> nativeCode_;
    std::string sqlState_;
    std::string constraintName_;
};

namespace detail {

class MariaDbPool;
class PostgreSqlPool;
class DbRegistry;
class DbMigrationRunner;
struct DbValueAccess;
struct DbResultAccess;

enum class DbValueType : std::uint8_t { kNull, kString, kSigned, kUnsigned, kDouble, kBool };

}  // namespace detail

class DbConversionError final : public std::runtime_error {
public:
    enum class Code : std::uint8_t {
        kInvalidFormat,
        kOutOfRange,
    };

    DbConversionError(Code code, std::string message)
        : std::runtime_error(std::move(message)),
          code_(code) {}

    [[nodiscard]] Code code() const noexcept {
        return code_;
    }

private:
    Code code_;
};

class DbValue final {
private:
    using Storage = std::variant<std::monostate, BorrowedText, std::pmr::string, std::int64_t,
        std::uint64_t, double, bool>;

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
        requires(std::is_integral_v<std::remove_cvref_t<T>> &&
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
    [[nodiscard]] std::string_view text() const& noexcept;
    [[nodiscard]] std::string_view text() const&& = delete;
    [[nodiscard]] std::int64_t signedValue() const noexcept;
    [[nodiscard]] std::uint64_t unsignedValue() const noexcept;
    [[nodiscard]] double doubleValue() const noexcept;
    [[nodiscard]] bool boolValue() const noexcept;

    template <typename T>
    [[nodiscard]] static Storage makeIntegerStorage(T value) {
        if constexpr (std::is_signed_v<std::remove_cvref_t<T>>) {
            return Storage(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
        } else {
            return Storage(std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(value));
        }
    }

    Storage storage_;
};

class DbField final {
private:
    using Storage = std::variant<std::monostate, std::pmr::string, BorrowedText>;

public:
    DbField(DbField&& other) noexcept;
    DbField& operator=(DbField&& other);

    DbField(const DbField&) = delete;
    DbField& operator=(const DbField&) = delete;

    [[nodiscard]] std::optional<std::string_view> value() const& noexcept;
    [[nodiscard]] std::optional<std::string_view> value() const&& = delete;

    template <typename T>
        requires(std::is_same_v<T, std::remove_cv_t<T>> &&
                 (std::is_same_v<T, bool> || (std::is_integral_v<T> && !std::is_same_v<T, char>) ||
                     std::is_floating_point_v<T> || std::is_same_v<T, std::string> ||
                     std::is_same_v<T, std::string_view>))
    [[nodiscard]] std::optional<T> as() const& {
        const auto source = value();
        if (!source) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, std::string_view>) {
            return *source;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return std::string(*source);
        } else if constexpr (std::is_same_v<T, bool>) {
            if (*source == "1" || *source == "t" || *source == "true" || *source == "TRUE") {
                return true;
            }
            if (*source == "0" || *source == "f" || *source == "false" || *source == "FALSE") {
                return false;
            }
            throw DbConversionError(
                DbConversionError::Code::kInvalidFormat, "database field is not a boolean");
        } else {
            T converted{};
            const auto* first = source->data();
            const auto* last = first + source->size();
            const auto [end, error] = std::from_chars(first, last, converted);
            if (error != std::errc{} || end != last) {
                throw DbConversionError(error == std::errc::result_out_of_range
                                            ? DbConversionError::Code::kOutOfRange
                                            : DbConversionError::Code::kInvalidFormat,
                    "database field has an invalid numeric value");
            }
            return converted;
        }
    }

    template <typename T>
        requires(std::is_same_v<T, std::remove_cv_t<T>> &&
                    (std::is_same_v<T, bool> ||
                        (std::is_integral_v<T> && !std::is_same_v<T, char>) ||
                        std::is_floating_point_v<T> || std::is_same_v<T, std::string> ||
                        std::is_same_v<T, std::string_view>))
    [[nodiscard]] std::optional<T> as() const&& = delete;

private:
    friend struct detail::DbResultAccess;

    struct BorrowedTag final {};

    explicit DbField(std::pmr::memory_resource* resource);
    DbField(std::nullptr_t, std::pmr::memory_resource* resource);
    DbField(std::string_view value, std::pmr::memory_resource* resource);
    DbField(BorrowedTag, std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static DbField borrowed(
        std::string_view value, std::pmr::memory_resource* resource);

    std::pmr::memory_resource* resource_;
    Storage storage_;
};

class DbRow final {
private:
    using OwnedFields = std::pmr::vector<DbField>;
    using BorrowedFields = std::span<const DbField>;
    using Storage = std::variant<OwnedFields, BorrowedFields>;
    using OwnedColumnNames = std::pmr::vector<std::pmr::string>;
    using BorrowedColumnNames = std::span<const std::pmr::string>;
    using ColumnNameStorage = std::variant<OwnedColumnNames, BorrowedColumnNames>;

public:
    DbRow(DbRow&& other) noexcept;
    DbRow& operator=(DbRow&& other);

    DbRow(const DbRow&) = delete;
    DbRow& operator=(const DbRow&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DbField& operator[](std::size_t index) const& noexcept;
    [[nodiscard]] const DbField& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const DbField& operator[](std::string_view column) const&;
    [[nodiscard]] const DbField& operator[](std::string_view column) const&& = delete;
    [[nodiscard]] const DbField* begin() const& noexcept;
    [[nodiscard]] const DbField* begin() const&& = delete;
    [[nodiscard]] const DbField* end() const& noexcept;
    [[nodiscard]] const DbField* end() const&& = delete;

private:
    friend struct detail::DbResultAccess;

    explicit DbRow(std::pmr::memory_resource* resource = nullptr);
    DbRow(const DbField* fields, std::size_t size, const std::pmr::string* columnNames,
        std::size_t columnCount, std::pmr::memory_resource* resource);
    [[nodiscard]] OwnedFields& ownedFields() noexcept;
    [[nodiscard]] OwnedColumnNames& ownedColumnNames() noexcept;
    [[nodiscard]] std::span<const std::pmr::string> columnNames() const noexcept;

    std::pmr::memory_resource* resource_;
    Storage storage_;
    ColumnNameStorage columnNames_;
};

}  // namespace ruvia
