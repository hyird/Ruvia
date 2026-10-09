#pragma once

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
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/attributes.h"
#include "ruvia/web/client_tls_config.h"

namespace ruvia {

class request_memory;
class db_value;

enum class db_driver : std::uint8_t {
    unspecified,
    mariadb,
    postgresql,
};

enum class db_transaction_isolation : std::uint8_t {
    default_value,
    read_uncommitted,
    read_committed,
    repeatable_read,
    serializable,
};

enum class db_transaction_access_mode : std::uint8_t {
    default_value,
    read_write,
    read_only,
};

struct db_transaction_options final {
    db_transaction_isolation isolation_{db_transaction_isolation::default_value};
    db_transaction_access_mode access_mode_{db_transaction_access_mode::default_value};
};

struct db_config final {
    db_driver driver_{db_driver::unspecified};
    // Host name or unbracketed address only; keep the port in port.
    std::string host_{"127.0.0.1"};
    // Absence selects the driver's standard port: 3306 or 5432.
    std::optional<std::uint16_t> port_{};
    std::string username_{};
    std::string password_{};
    client_tls_config tls_{};
    std::string database_{};
    // Absence disables the corresponding timeout.
    std::optional<std::chrono::milliseconds> connect_timeout_{std::chrono::seconds(5)};
    std::optional<std::chrono::milliseconds> read_timeout_{};
    std::optional<std::chrono::milliseconds> write_timeout_{};
    std::optional<std::chrono::milliseconds> query_timeout_{std::chrono::seconds(30)};
    std::optional<std::chrono::milliseconds> acquire_timeout_{std::chrono::seconds(5)};
};

class db_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        not_configured,
        resolve_failed,
        connect_failed,
        io_error,
        statement_failed,
        protocol_error,
        timeout,
        cancelled,
        closing,
    };

    db_error(code_type code, std::optional<db_driver> driver, const std::string& message,
        std::optional<std::int64_t> native_code = std::nullopt, std::string sql_state = {},
        std::string constraint_name = {})
        : std::runtime_error(message),
          code_(code),
          driver_(driver),
          native_code_(native_code),
          sql_state_(std::move(sql_state)),
          constraint_name_(std::move(constraint_name)) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::optional<db_driver> driver() const noexcept {
        return driver_;
    }

    [[nodiscard]] std::optional<std::int64_t> native_code() const noexcept {
        return native_code_;
    }

    [[nodiscard]] std::optional<std::string_view> sql_state() const& noexcept {
        if (sql_state_.empty()) {
            return std::nullopt;
        }
        return sql_state_;
    }

    [[nodiscard]] std::optional<std::string_view> sql_state() const&& = delete;

    [[nodiscard]] std::optional<std::string_view> constraint_name() const& noexcept {
        if (constraint_name_.empty()) {
            return std::nullopt;
        }
        return constraint_name_;
    }

    [[nodiscard]] std::optional<std::string_view> constraint_name() const&& = delete;

private:
    code_type code_;
    std::optional<db_driver> driver_;
    std::optional<std::int64_t> native_code_;
    std::string sql_state_;
    std::string constraint_name_;
};

namespace detail {

class mariadb_pool;
class postgresql_pool;
class db_registry;
class db_migration_runner;
struct db_value_access;
struct db_result_access;

enum class db_value_type : std::uint8_t { null,
    string,
    signed_value,
    unsigned_value,
    double_value,
    bool_value };

}  // namespace detail

class db_conversion_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        invalid_format,
        out_of_range,
    };

    db_conversion_error(code_type code, const std::string& message)
        : std::runtime_error(message),
          code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

class db_value final {
private:
    using storage_type = std::variant<std::monostate, borrowed_text, std::pmr::string, std::int64_t,
        std::uint64_t, double, bool>;

public:
    db_value(std::nullptr_t);
    // Text is borrowed until a database operation synchronously clones the
    // parameter. The source must outlive this value; owning-string rvalues are
    // rejected so a stored db_value cannot retain a destroyed temporary.
    db_value(const char* value);
    db_value(std::string_view value);

    template <typename traits_type, typename allocator_type>
    db_value(std::basic_string<char, traits_type, allocator_type>&&) = delete;

    template <typename traits_type, typename allocator_type>
    db_value(const std::basic_string<char, traits_type, allocator_type>&&) = delete;

    db_value(bool value);

    db_value(const db_value&) = default;
    db_value(db_value&&) noexcept = default;
    db_value& operator=(const db_value&) = delete;
    db_value& operator=(db_value&&) = delete;

    template <typename t_type>
        requires(std::is_integral_v<std::remove_cvref_t<t_type>> &&
                 !std::is_same_v<std::remove_cvref_t<t_type>, bool>)
    db_value(t_type value)
        : storage_(make_integer_storage(value)) {}

    template <typename t_type>
        requires std::is_floating_point_v<std::remove_cvref_t<t_type>>
    db_value(t_type value)
        : storage_(std::in_place_type<double>, static_cast<double>(value)) {}

private:
    friend struct detail::db_value_access;

    explicit db_value(std::pmr::string value);

    [[nodiscard]] detail::db_value_type type() const noexcept;
    [[nodiscard]] std::string_view text() const& noexcept;
    [[nodiscard]] std::string_view text() const&& = delete;
    [[nodiscard]] std::int64_t signed_value() const noexcept;
    [[nodiscard]] std::uint64_t unsigned_value() const noexcept;
    [[nodiscard]] double get_double_value() const noexcept;
    [[nodiscard]] bool get_bool_value() const noexcept;

    template <typename t_type>
    [[nodiscard]] static storage_type make_integer_storage(t_type value) {
        if constexpr (std::is_signed_v<std::remove_cvref_t<t_type>>) {
            return storage_type(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(value));
        } else {
            return storage_type(std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(value));
        }
    }

    storage_type storage_;
};

class db_field final {
private:
    using storage_type = std::variant<std::monostate, std::pmr::string, borrowed_text>;

public:
    db_field(db_field&& other) noexcept;
    // A different PMR resource can require allocation during assignment.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    db_field& operator=(db_field&& other);

    db_field(const db_field&) = delete;
    db_field& operator=(const db_field&) = delete;

    [[nodiscard]] std::optional<std::string_view> value() const& noexcept;
    [[nodiscard]] std::optional<std::string_view> value() const&& = delete;

    template <typename t_type>
        requires(std::is_same_v<t_type, std::remove_cv_t<t_type>> &&
                 (std::is_same_v<t_type, bool> || (std::is_integral_v<t_type> && !std::is_same_v<t_type, char>) ||
                     std::is_floating_point_v<t_type> || std::is_same_v<t_type, std::string> ||
                     std::is_same_v<t_type, std::string_view>))
    [[nodiscard]] std::optional<t_type> as() const& {
        const auto source_value = value();
        if (!source_value) {
            return std::nullopt;
        }
        if constexpr (std::is_same_v<t_type, std::string_view>) {
            return *source_value;
        } else if constexpr (std::is_same_v<t_type, std::string>) {
            return std::string(*source_value);
        } else if constexpr (std::is_same_v<t_type, bool>) {
            if (*source_value == "1" || *source_value == "t" || *source_value == "true" || *source_value == "TRUE") {
                return true;
            }
            if (*source_value == "0" || *source_value == "f" || *source_value == "false" || *source_value == "FALSE") {
                return false;
            }
            throw db_conversion_error(
                db_conversion_error::code_type::invalid_format, "database field is not a boolean");
        } else {
            t_type converted{};
            const auto* first = source_value->data();
            const auto* last = first + source_value->size();
            const auto [end, error] = std::from_chars(first, last, converted);
            if (error != std::errc{} || end != last) {
                throw db_conversion_error(error == std::errc::result_out_of_range
                                              ? db_conversion_error::code_type::out_of_range
                                              : db_conversion_error::code_type::invalid_format,
                    "database field has an invalid numeric value");
            }
            return converted;
        }
    }

    template <typename t_type>
        requires(std::is_same_v<t_type, std::remove_cv_t<t_type>> &&
                    (std::is_same_v<t_type, bool> ||
                        (std::is_integral_v<t_type> && !std::is_same_v<t_type, char>) ||
                        std::is_floating_point_v<t_type> || std::is_same_v<t_type, std::string> ||
                        std::is_same_v<t_type, std::string_view>))
    [[nodiscard]] std::optional<t_type> as() const&& = delete;

private:
    friend struct detail::db_result_access;
    friend class db_row;

    struct borrowed_tag_type final {};

    explicit db_field(std::pmr::memory_resource* resource);
    db_field(const db_field& other, std::pmr::memory_resource* resource);
    db_field(std::nullptr_t, std::pmr::memory_resource* resource);
    db_field(std::string_view value, std::pmr::memory_resource* resource);
    db_field(borrowed_tag_type, std::string_view value, std::pmr::memory_resource* resource);
    [[nodiscard]] static db_field borrowed(
        std::string_view value, std::pmr::memory_resource* resource);

    std::pmr::memory_resource* resource_;
    storage_type storage_;
};

class db_row final {
private:
    using owned_fields_type = std::pmr::vector<db_field>;
    using borrowed_fields_type = std::span<const db_field>;
    using storage_type = std::variant<owned_fields_type, borrowed_fields_type>;
    using owned_column_names_type = std::pmr::vector<std::pmr::string>;
    using borrowed_column_names_type = std::span<const std::pmr::string>;
    using column_name_storage_type = std::variant<owned_column_names_type, borrowed_column_names_type>;

public:
    db_row(db_row&& other) noexcept;
    // Assignment retains the destination PMR resource. Allocation failure
    // leaves both rows unchanged.
    // NOLINTNEXTLINE(performance-noexcept-move-constructor)
    db_row& operator=(db_row&& other);

    db_row(const db_row&) = delete;
    db_row& operator=(const db_row&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const db_field& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const db_field& operator[](std::string_view column) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field& operator[](std::string_view column) const&& = delete;
    [[nodiscard]] const db_field* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field* begin() const&& = delete;
    [[nodiscard]] const db_field* end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field* end() const&& = delete;

private:
    friend struct detail::db_result_access;

    explicit db_row(std::pmr::memory_resource* resource = nullptr);
    db_row(const db_field* fields_value, std::size_t size, const std::pmr::string* column_names,
        std::size_t column_count, std::pmr::memory_resource* resource);
    [[nodiscard]] owned_fields_type& owned_fields() noexcept;
    [[nodiscard]] owned_column_names_type& owned_column_names() noexcept;
    [[nodiscard]] std::span<const std::pmr::string> column_names() const noexcept;

    std::pmr::memory_resource* resource_;
    storage_type storage_;
    column_name_storage_type column_names_;
};

}  // namespace ruvia
