#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/port_number.h"
#include "ruvia/web/detail/text_scalar.h"
#include "ruvia/web/model.h"
#include "ruvia/web/tls_config.h"

namespace ruvia {

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

enum class db_data_type : unsigned char { inferred,
    boolean,
    small_int,
    integer,
    big_int,
    real,
    double_value,
    text,
    char_value,
    varchar,
    numeric,
    json,
    jsonb,
    uuid,
    date,
    timestamp,
    timestamp_tz,
    interval,
    inet,
    cidr,
    bytea,
    array };

// Each worker owns a fixed pool per database alias. Standalone db_client uses
// one pool on its bound event loop; db_migrator uses a dedicated connection.
// PostgreSQL query() and execute() keep a bounded set of named server-side
// prepared statements per connection, so the server may reuse a generic plan
// (see its plan_cache_mode); a pooler between client and server must support
// protocol-level prepared statements. Streams run as unnamed statements.
// Outside a transaction, concurrent PostgreSQL query() and execute() calls on
// one worker share its connections in pipeline mode, several statements in
// flight on one connection, each ending its own implicit transaction; session
// state one of them changes (SET, an explicit BEGIN) is seen by the others.
struct db_config final {
    db_driver driver_{db_driver::unspecified};
    // Host name or unbracketed address only; keep the port in port.
    std::string host_{"127.0.0.1"};
    // Absence selects the driver's standard port: 3306 or 5432. Explicit values
    // are checked before narrowing and must be non-zero.
    std::optional<port_number> port_{};
    std::string username_;
    std::string password_;
    client_tls_config tls_{};
    std::string database_;
    // Opened at startup; must be positive. Total application connections per
    // alias are worker_count * pool_size_per_worker. A transaction or streaming
    // result holds its connection until completed or released.
    std::size_t pool_size_per_worker_{1};
    // Absence disables the corresponding timeout. Integer chrono inputs are
    // rounded up to milliseconds and saturated before any signed narrowing.
    std::optional<timeout_duration> connect_timeout_{std::chrono::seconds(5)};
    // Bound one socket wait for readability or writability, for both drivers,
    // with millisecond precision (including exclusive transactions and streams).
    std::optional<timeout_duration> read_timeout_{};
    std::optional<timeout_duration> write_timeout_{};
    std::optional<timeout_duration> query_timeout_{std::chrono::seconds(30)};
    std::optional<timeout_duration> acquire_timeout_{std::chrono::seconds(5)};
    // Overall deadline of one connection lease: a transaction from the moment
    // begin_transaction() holds its connection (before BEGIN) through commit()
    // or rollback(), and a streaming result from query_stream() until it is
    // exhausted or closed. Every statement inside runs under the earlier of its
    // own deadline (operation timeout and query_timeout_) and the remaining
    // lease time. Expiry while a statement runs fails that statement with
    // db_error::code_type::timeout and ends the transaction or stream. Expiry
    // while the caller holds the lease between operations closes the
    // connection at once, so the server rolls the transaction back and
    // releases its locks; the next operation, commit() included, then fails
    // with timeout. The connection is never returned to the pool after expiry.
    // db_migrator applies the same value to transactional migrations.
    std::optional<timeout_duration> transaction_timeout_{std::chrono::seconds(60)};
};

// Query-result caching stores rows in Redis. The types are declared in every
// build so the public layout does not depend on feature macros; registering a
// cache in a build without Redis support is rejected at configuration time.
// A result whose entry the Redis capability's max_reply_bytes_ could not read
// back is returned but not stored. Concurrent misses of one key on one worker
// run one database query; the other callers wait for it within their own
// timeout and stop token.
struct db_cache_config final {
    std::chrono::milliseconds duration_{1000};
    bool always_enabled_{false};
    bool ignore_errors_{false};
    // Use a distinct namespace for databases sharing one Redis database.
    std::string key_prefix_{"ruvia:query-result-cache"};
};

// application startup resolves this alias to its existing worker-local Redis capability.
struct db_query_cache_registration final {
    std::string redis_alias_{"default"};
    db_cache_config policy_{};
};

struct db_cache_options final {
    borrowed_text id_{};
    std::optional<std::chrono::milliseconds> duration_{};
};

using db_cache_setting_type = std::variant<std::monostate, bool, std::chrono::milliseconds, db_cache_options>;

namespace detail {

struct mariadb_driver;
struct postgresql_driver;
template <typename driver_type>
class db_pool;

// Closed, allocation-free reference to one concrete database pool. Database
// operations cross exactly one explicit branch; there is no virtual dispatch,
// shared ownership, or request-path allocation.
using db_pool_ref_type = std::variant<std::monostate, db_pool<mariadb_driver>*, db_pool<postgresql_driver>*>;

inline void append_db_number(std::string& output, std::int64_t value) {
    http_append_signed(output, value);
}

inline void append_db_number(std::string& output, std::uint64_t value) {
    http_append_unsigned(output, value);
}

template <std::floating_point number_type>
inline void append_db_number(std::string& output, number_type value) {
    // SQL numeric literals cannot contain the inf/nan spellings of to_chars.
    http_append_formatted_finite_number(output, value, "database floating-point value must be finite",
        "database floating-point value cannot be formatted");
}

// The result shapes a repository builder derives from one query. An id-keyed
// cache entry ignores the SQL, so its key holds the caller's cache id and the
// shape as separate parts, and removing that id removes every shape.
enum class db_cache_shape : std::uint8_t {
    rows,
    one,
    exists,
    count,
};

}  // namespace detail

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

    db_error(code_type code, std::optional<db_driver> driver, std::string_view message,
        std::optional<std::int64_t> native_code = std::nullopt, std::string_view sql_state = {},
        std::string_view constraint_name = {})
        : std::runtime_error(std::string(message)),
          code_(code),
          driver_(driver),
          native_code_(native_code),
          sql_state_(sql_state),
          constraint_name_(constraint_name) {}

    db_error(const db_error& other)
        : std::runtime_error(other),
          code_(other.code_),
          driver_(other.driver_),
          native_code_(other.native_code_),
          sql_state_(other.sql_state_, other.sql_state_.get_allocator()),
          constraint_name_(other.constraint_name_, other.constraint_name_.get_allocator()) {}
    db_error(db_error&&) = default;
    db_error& operator=(const db_error&) = default;
    db_error& operator=(db_error&&) = default;

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

    [[nodiscard]] std::optional<db_driver> driver() const noexcept {
        return driver_;
    }

    [[nodiscard]] std::optional<std::int64_t> native_code() const noexcept {
        return native_code_;
    }

    [[nodiscard]] std::optional<std::string_view> sql_state() const& noexcept RUVIA_LIFETIMEBOUND {
        if (sql_state_.empty()) {
            return std::nullopt;
        }
        return sql_state_;
    }

    [[nodiscard]] std::optional<std::string_view> sql_state() const&& = delete;

    [[nodiscard]] std::optional<std::string_view> constraint_name() const& noexcept RUVIA_LIFETIMEBOUND {
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

class db_registry;
class db_migration_runner;
struct db_value_access;
struct db_result_access;

}  // namespace detail

enum class db_value_type : std::uint8_t { null,
    string,
    signed_value,
    unsigned_value,
    double_value,
    bool_value };

class db_conversion_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        invalid_format,
        out_of_range,
        unexpected_null,
        missing_column,
        unexpected_row_count,
    };

    db_conversion_error(code_type code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

namespace detail {

template <typename t_type, bool allow_array = true>
struct db_mapped_value_traits : std::bool_constant<text_scalar<t_type> && !std::is_same_v<t_type, std::string_view>> {};

template <typename t_type, bool allow_array>
struct db_mapped_value_traits<std::optional<t_type>, allow_array> : db_mapped_value_traits<t_type, allow_array> {};

template <typename t_type, bool allow_array>
struct db_mapped_value_traits<std::vector<t_type>, allow_array>
    : std::bool_constant<allow_array && db_mapped_value_traits<t_type, false>::value> {};

// The one text-to-scalar conversion for database results: fields and array
// elements accept the same boolean tokens and numeric syntax.
template <text_scalar t_type>
[[nodiscard]] t_type convert_db_text(std::string_view text) {
    auto parsed = parse_text_scalar<t_type>(text);
    if (const auto* error = std::get_if<text_scalar_parse_error>(&parsed)) {
        if constexpr (std::is_same_v<t_type, bool>) {
            throw db_conversion_error(db_conversion_error::code_type::invalid_format, "database text is not a boolean");
        } else {
            throw db_conversion_error(*error == text_scalar_parse_error::out_of_range
                                          ? db_conversion_error::code_type::out_of_range
                                          : db_conversion_error::code_type::invalid_format,
                "database text is not a valid number");
        }
    }
    return std::get<t_type>(std::move(parsed));
}

}  // namespace detail

// Mapped SQL values own their text independently of db_rows: numbers, bool,
// std::string, ruvia::string, and one-dimensional std::vector values.
// Optional wrappers preserve ownership, including nullable array elements;
// borrowed views, references, and nested arrays are not mapped value types.
template <typename t_type>
concept db_mapped_value = detail::db_mapped_value_traits<t_type>::value;

class db_value final {
private:
    using storage_type = std::variant<std::monostate, borrowed_text, std::string, std::int64_t,
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

    db_value(const db_value& other);
    db_value(db_value&&) noexcept = default;
    db_value& operator=(const db_value&) = delete;
    db_value& operator=(db_value&&) = delete;

    template <typename t_type>
        requires detail::is_model_integer<std::remove_cvref_t<t_type>>
    db_value(t_type value)
        : storage_(make_integer_storage(value)) {}

    // 有限输入超出 double 范围时在构造阶段抛出 std::out_of_range，
    // 不先窄化成无穷大。普通精度舍入及显式非有限输入的后续校验保持不变。
    template <typename t_type>
        requires std::is_floating_point_v<std::remove_cvref_t<t_type>>
    db_value(t_type value)
        : storage_(std::in_place_type<double>, detail::checked_model_float<double>(value)) {}

    // Reading a different alternative throws std::bad_variant_access; values
    // are never coerced. Text borrows this value and, for borrowed inputs,
    // the original source. Compiled statements own their parameter text.
    [[nodiscard]] db_value_type type() const noexcept;
    [[nodiscard]] std::string_view text() const& RUVIA_LIFETIMEBOUND;
    std::string_view text() const&& = delete;
    [[nodiscard]] std::int64_t signed_value() const;
    [[nodiscard]] std::uint64_t unsigned_value() const;
    [[nodiscard]] double double_value() const;
    [[nodiscard]] bool bool_value() const;

private:
    friend struct detail::db_value_access;

    // Tagged so it never competes with the public borrowing constructors: a
    // std::string lvalue argument still borrows through string_view.
    db_value(std::in_place_type_t<std::string>, std::string value);

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

namespace detail {

struct db_value_access final {
    [[nodiscard]] static db_value owned_string(std::string value) {
        return db_value(std::in_place_type<std::string>, std::move(value));
    }

    // Prepared statement parameters own their text. Requiring that alternative
    // gives native text protocols a checked, NUL-terminated borrowing boundary.
    [[nodiscard]] static const std::string& owned_text(const db_value& value) {
        return std::get<std::string>(value.storage_);
    }
    static const std::string& owned_text(const db_value&&) = delete;
};

template <typename t_type>
inline constexpr bool db_owning_char_string = false;

template <typename traits_type, typename allocator_type>
inline constexpr bool db_owning_char_string<std::basic_string<char, traits_type, allocator_type>> = true;

// One value the variadic query()/execute() overloads accept. A type that
// already denotes a whole parameter sequence -- std::span, std::array, or a
// vector of db_value -- cannot construct a db_value, so it fails this concept and
// the span overload keeps winning without needing an explicit exclusion.
template <typename param_type>
concept db_parameter =
    std::constructible_from<db_value, param_type&&> || db_owning_char_string<std::remove_cvref_t<param_type>>;

template <typename... params_type>
concept db_parameter_pack = sizeof...(params_type) > 0 && (db_parameter<params_type> && ...);

// DB entry points clone each value into their own storage before returning.
// Borrow owned db_value text and owning-string temporaries only across that
// synchronous boundary; db_value itself still rejects retained string temporaries.
template <typename param_type>
    requires db_parameter<param_type>
[[nodiscard]] db_value make_immediate_db_parameter(param_type&& param) {
    if constexpr (std::same_as<std::remove_cvref_t<param_type>, db_value>) {
        if (param.type() == db_value_type::string) {
            return db_value(param.text());
        }
        return db_value(std::forward<param_type>(param));
    } else if constexpr (db_owning_char_string<std::remove_cvref_t<param_type>>) {
        return db_value(std::string_view(param));
    } else {
        return db_value(std::forward<param_type>(param));
    }
}

}  // namespace detail
class db_field final {
private:
    using storage_type = std::variant<std::monostate, std::string, borrowed_text>;

public:
    db_field(db_field&& other) noexcept;
    db_field& operator=(db_field&& other) noexcept;

    db_field(const db_field&) = delete;
    db_field& operator=(const db_field&) = delete;

    [[nodiscard]] std::optional<std::string_view> value() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> value() const&& = delete;

    [[nodiscard]] bool is_null() const noexcept {
        return !value().has_value();
    }
    // string_view is a raw borrow, valid only while this field's row storage
    // remains alive. Repository mapped results instead require db_mapped_value.
    template <detail::text_scalar t_type>
    [[nodiscard]] std::optional<t_type> as() const& {
        const auto source_value = value();
        if (!source_value) {
            return std::nullopt;
        }
        return detail::convert_db_text<t_type>(*source_value);
    }

    template <detail::text_scalar t_type>
    [[nodiscard]] std::optional<t_type> as() const&& = delete;

private:
    friend struct detail::db_result_access;
    friend class db_row;
    friend class db_rows;

    struct borrowed_tag_type final {};

    db_field() noexcept;
    explicit db_field(std::nullptr_t) noexcept;
    explicit db_field(std::string_view value);
    db_field(borrowed_tag_type, std::string_view value) noexcept;
    [[nodiscard]] static db_field borrowed(std::string_view value) noexcept;

    storage_type storage_;
};

class db_row final {
public:
    db_row(db_row&& other) noexcept;
    db_row& operator=(db_row&& other) noexcept;

    db_row(const db_row&) = delete;
    db_row& operator=(const db_row&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const db_field& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const db_field& operator[](std::string_view column) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field& operator[](std::string_view column) const&& = delete;
    [[nodiscard]] bool contains(std::string_view column) const noexcept;
    [[nodiscard]] const db_field* find(std::string_view column) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field* find(std::string_view column) const&& = delete;
    [[nodiscard]] std::span<const std::string> column_names() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> column_names() const&& = delete;
    [[nodiscard]] const db_field* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field* begin() const&& = delete;
    [[nodiscard]] const db_field* end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_field* end() const&& = delete;

private:
    friend struct detail::db_result_access;
    friend class db_rows;

    db_row() noexcept = default;
    db_row(const db_field* fields_value, std::size_t size, const std::string* column_names,
        std::size_t column_count) noexcept;

    std::span<const db_field> fields_{};
    std::span<const std::string> column_names_{};
};

}  // namespace ruvia
