#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ruvia {

class RequestMemory;

struct DbConfig {
    // Host name or unbracketed address only; keep the port in port.
    std::pmr::string host{"127.0.0.1"};
    // Must be non-zero.
    std::uint16_t port{3306};
    std::pmr::string username;
    std::pmr::string password;
    std::pmr::string database;
    // Number of connections per worker; must be greater than zero.
    std::size_t poolSize{4};
    // Set to 0 to disable the corresponding timeout.
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
    DbField(DbField&& other) noexcept;
    DbField& operator=(DbField&& other) noexcept;

    DbField(const DbField&) = delete;
    DbField& operator=(const DbField&) = delete;

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;

private:
    friend class detail::MariaDbPool;

    struct BorrowedTag final {};

    explicit DbField(std::pmr::memory_resource* resource);
    DbField(std::nullptr_t, std::pmr::memory_resource* resource);
    DbField(std::string_view value, std::pmr::memory_resource* resource);
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
    DbRow(DbRow&& other) noexcept;
    DbRow& operator=(DbRow&& other) noexcept;

    DbRow(const DbRow&) = delete;
    DbRow& operator=(const DbRow&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DbField& operator[](std::size_t index) const noexcept;
    [[nodiscard]] const DbField* begin() const noexcept;
    [[nodiscard]] const DbField* end() const noexcept;

private:
    friend class detail::MariaDbPool;

    explicit DbRow(std::pmr::memory_resource* resource = nullptr);
    DbRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource);
    void refreshView() noexcept;

    std::pmr::vector<DbField> ownedFields_;
    const DbField* fields_{nullptr};
    std::size_t size_{0};
    bool ownsFields_{true};
};

}  // namespace ruvia
