#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
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
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/http/url_encoding.h"
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

namespace ruvia {

namespace detail {
struct request_io_metrics;

// A database stream or transaction owns one pool lease. Cold guards only
// register lifetime obligations that forbid destroying their owner; they do
// not reserve the operation right. Operating is the sole state that grants
// that right. Closed and Failed are terminal states so a failed lease can
// never be reused accidentally.
template <typename payload>
class db_operation_guard;

template <typename payload>
class db_operation_state final {
public:
    struct closed_type final {};
    struct failed_type final {};

    struct active_type final {
        payload payload_;
    };

    struct operating_type final {
        payload payload_;
    };

    db_operation_state() noexcept = default;

    explicit db_operation_state(payload payload_value) noexcept
        : state_(std::in_place_type<active_type>, std::move(payload_value)) {
        static_assert(std::is_nothrow_move_constructible_v<payload>);
    }

    db_operation_state(const db_operation_state&) = delete;
    db_operation_state& operator=(const db_operation_state&) = delete;
    ~db_operation_state() {
        reset();
    }

    db_operation_state(db_operation_state&&) = delete;
    db_operation_state& operator=(db_operation_state&&) = delete;

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<active_type>(state_);
    }

    [[nodiscard]] bool finished() const noexcept {
        return std::holds_alternative<closed_type>(state_) || std::holds_alternative<failed_type>(state_);
    }
    // Preparation only borrows immutable connection and resource metadata. It
    // never grants the operation right, even while another task holds it.
    [[nodiscard]] const payload& preparation_payload() const {
        if (const auto* active = std::get_if<active_type>(&state_); active != nullptr) {
            return active->payload_;
        }
        if (const auto* operating = std::get_if<operating_type>(&state_); operating != nullptr) {
            return operating->payload_;
        }
        throw std::logic_error("database resource is not active");
    }

    void reset() noexcept {
        if (cold_borrows_ != 0 || std::holds_alternative<operating_type>(state_)) {
            // Destroying the database owner while any operation borrows it would
            // leave that coroutine with a dangling owner, even before start().
            std::terminate();
        }
        if (auto* active = std::get_if<active_type>(&state_); active != nullptr) {
            active->payload_.abort();
        }
        state_.template emplace<closed_type>();
    }

private:
    friend class db_operation_guard<payload>;

    void register_cold_borrow() noexcept {
        ++cold_borrows_;
    }

    void start() {
        auto* active = std::get_if<active_type>(&state_);
        if (active == nullptr) {
            throw std::logic_error(std::holds_alternative<operating_type>(state_)
                                       ? "database operation is already in progress"
                                       : "database resource is not active");
        }
        if (cold_borrows_ == 0) {
            std::terminate();
        }
        --cold_borrows_;
        payload payload_value(std::move(active->payload_));
        state_.template emplace<operating_type>(std::move(payload_value));
    }

    void release_cold_borrow() noexcept {
        if (cold_borrows_ == 0) {
            std::terminate();
        }
        --cold_borrows_;
    }

    [[nodiscard]] payload& operation_payload() noexcept {
        auto* operating = std::get_if<operating_type>(&state_);
        if (operating == nullptr) {
            std::terminate();
        }
        return operating->payload_;
    }

    void finish_active() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        auto& operating = std::get<operating_type>(state_);
        payload payload_value(std::move(operating.payload_));
        state_.template emplace<active_type>(std::move(payload_value));
    }

    void finish_closed() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        state_.template emplace<closed_type>();
    }

    void finish_failed() noexcept {
        if (!std::holds_alternative<operating_type>(state_)) {
            std::terminate();
        }
        state_.template emplace<failed_type>();
    }

    std::variant<closed_type, active_type, operating_type, failed_type> state_{};
    std::size_t cold_borrows_{0};
};

// One operation on a db_operation_state. Construction only registers a cold
// lifetime borrow. start() acquires the exclusive lease when the coroutine
// actually begins. Dropping a cold guard unregisters only its own borrow;
// destroying a started guard without naming an ending fails the lease.
template <typename payload>
class db_operation_guard final {
public:
    using state_type = db_operation_state<payload>;

    explicit db_operation_guard(state_type& state_value) noexcept
        : state_(&state_value) {
        state_->register_cold_borrow();
    }

    db_operation_guard(const db_operation_guard&) = delete;
    db_operation_guard& operator=(const db_operation_guard&) = delete;
    db_operation_guard(db_operation_guard&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          started_(std::exchange(other.started_, false)) {}
    db_operation_guard& operator=(db_operation_guard&&) = delete;

    ~db_operation_guard() {
        if (state_ != nullptr) {
            if (started_) {
                state_->finish_failed();
            } else {
                state_->release_cold_borrow();
            }
        }
    }

    void start() {
        state_->start();
        started_ = true;
    }

    [[nodiscard]] bool finished() const noexcept {
        return state_ != nullptr && state_->finished();
    }
    [[nodiscard]] payload& lease() noexcept {
        if (state_ == nullptr || !started_) {
            std::terminate();
        }
        return state_->operation_payload();
    }

    void finish_active() noexcept {
        state_->finish_active();
        state_ = nullptr;
    }

    void finish_closed() noexcept {
        state_->finish_closed();
        state_ = nullptr;
    }

    void finish_failed() noexcept {
        state_->finish_failed();
        state_ = nullptr;
    }

private:
    state_type* state_;
    bool started_{false};
};

}  // namespace detail

// Result of a statement whose contract is side effects rather than a row set.
// PostgreSQL does not expose a portable connection-level insert id, so that
// value is present only when the selected backend supplied one.
class db_exec_result final {
public:
    [[nodiscard]] constexpr std::uint64_t affected_rows() const noexcept {
        return affected_rows_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> last_insert_id() const noexcept {
        return last_insert_id_;
    }

private:
    friend struct detail::db_result_access;

    constexpr db_exec_result(
        std::uint64_t affected_rows, std::optional<std::uint64_t> last_insert_id) noexcept
        : affected_rows_(affected_rows),
          last_insert_id_(last_insert_id) {}

    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> last_insert_id_;
};

class db_rows final {
public:
    db_rows(const db_rows&) = delete;
    db_rows& operator=(const db_rows&) = delete;
    db_rows(db_rows&& other) noexcept;
    db_rows& operator=(db_rows&& other);
    ~db_rows();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const db_row& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& operator[](std::size_t index) const&& = delete;
    [[nodiscard]] const db_row* begin() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* begin() const&& = delete;
    [[nodiscard]] const db_row* end() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row* end() const&& = delete;
    [[nodiscard]] const db_row& front() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& front() const&& = delete;
    [[nodiscard]] const db_row& back() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const db_row& back() const&& = delete;
    [[nodiscard]] std::span<const std::string> column_names() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> column_names() const&& = delete;

private:
    friend struct detail::db_result_access;

    struct no_raw_result_type final {};

    struct owned_raw_result_type final {
        owned_raw_result_type(void* owned_value, void (*owned_release)(void*) noexcept) noexcept
            : value_(owned_value),
              release_(owned_release) {}

        void* value_;
        void (*release_)(void*) noexcept;
    };

    db_rows() noexcept;

    std::vector<db_row> rows_;
    std::vector<db_field> fields_;
    std::vector<std::string> column_names_;
    std::variant<no_raw_result_type, owned_raw_result_type> raw_result_;
};

class db_stream_result final {
public:
    db_stream_result(const db_stream_result&) = delete;
    db_stream_result& operator=(const db_stream_result&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    db_stream_result(db_stream_result&& other) noexcept;
    db_stream_result& operator=(db_stream_result&&) = delete;
    ~db_stream_result();

    [[nodiscard]] bool active() const noexcept;
    // Available when query_stream() completes, including empty row sets.
    // Names remain valid through reads, EOF and close, until this state dies.
    [[nodiscard]] std::span<const std::string> column_names() const& RUVIA_LIFETIMEBOUND;
    std::span<const std::string> column_names() const&& = delete;
    // Each read has one operation deadline, including any EOF/trailing-result
    // drain needed before returning nullopt. A separate close starts its own
    // deadline. Both end no later than the stream's overall deadline
    // (db_config::transaction_timeout_), which runs from query_stream() until
    // the stream ends and also counts the time between operations; once it has
    // passed, the connection is closed and read() and close() fail with
    // db_error::code_type::timeout.
    // `operation` refines the handle's options for this call only: its timeout
    // may only tighten, and a supplied token replaces the inherited one. A
    // non-positive timeout throws std::invalid_argument at the call; an
    // already-stopped token throws db_error(cancelled) and leaves the stream
    // usable. Once started, a timeout or stop request closes the connection --
    // a pooled connection interrupted mid-result is never reused -- and ends
    // the stream with db_error timeout / cancelled.
    [[nodiscard]] task<std::optional<db_row>> read(operation_options operation = {}) &;
    task<std::optional<db_row>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> close(operation_options operation = {}) &;
    task<void> close(operation_options = {}) && = delete;

private:
    friend class db_session;
    template <typename driver_type>
    friend class detail::db_pool;

    // The driver's result handle lives in the pool slot, so the pool alone
    // frees it, including when the stream's deadline expires between reads.
    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot, operation_options options) noexcept;
        void abort() noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        std::vector<db_field> fields_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    struct state_type final {
        state_type(lease_type lease, std::vector<std::string> column_names) noexcept;
        std::vector<std::string> column_names_;
        operation_state_type operation_;
    };
    using state_owner_type = std::unique_ptr<state_type>;

    db_stream_result() noexcept = default;
    db_stream_result(detail::db_pool_ref_type client, std::size_t slot, std::vector<std::string> column_names,
        operation_options options);
    void reset() noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    void bind_operation_scope(::ruvia::operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept;
    static void expire_capability(void* target) noexcept;
    static task<std::optional<db_row>> read_task(detail::request_io_metrics* io_metrics,
        operation_guard_type operation, std::vector<std::string>& column_names, operation_options operation_value);
    static task<void> close_task(detail::request_io_metrics* io_metrics, operation_guard_type operation, operation_options operation_value);

    state_owner_type state_;
    detail::request_io_metrics* io_metrics_{nullptr};
    scoped_capability_registration registration_;
};

namespace detail {

// The public result objects stay backend-neutral. Concrete drivers populate
// them through this single internal access point instead of accumulating one
// friend declaration per driver.
struct db_result_access final {
    template <typename entity_type>
    [[nodiscard]] static entity_rows<entity_type> make_entity_rows(std::size_t size) {
        entity_rows<entity_type> result;
        result.reserve(size);
        return result;
    }

    [[nodiscard]] static db_rows make_result() noexcept {
        return db_rows();
    }

    [[nodiscard]] static constexpr db_exec_result make_exec_result(std::uint64_t affected_rows,
        std::optional<std::uint64_t> last_insert_id = std::nullopt) noexcept {
        return db_exec_result(affected_rows, last_insert_id);
    }

    [[nodiscard]] static std::vector<db_row>& rows(db_rows& result_value) noexcept {
        return result_value.rows_;
    }

    [[nodiscard]] static std::vector<db_field>& fields(db_rows& result_value) noexcept {
        return result_value.fields_;
    }

    [[nodiscard]] static std::vector<std::string>& column_names(db_rows& result_value) noexcept {
        return result_value.column_names_;
    }

    // Leaves a result whose rows could not be kept empty, so a failed
    // materialization never hands out part of a result.
    static void clear(db_rows& result_value) noexcept {
        result_value.rows_.clear();
        result_value.fields_.clear();
        result_value.column_names_.clear();
    }

    static void own_raw_result(db_rows& result_value, void* raw, void (*release)(void*) noexcept) noexcept {
        if (raw == nullptr || release == nullptr ||
            std::holds_alternative<db_rows::owned_raw_result_type>(result_value.raw_result_)) {
            std::terminate();
        }
        result_value.raw_result_.template emplace<db_rows::owned_raw_result_type>(raw, release);
    }

    [[nodiscard]] static db_field null_field() noexcept {
        return db_field(nullptr);
    }

    [[nodiscard]] static db_field owned_field(std::string_view value) {
        return db_field(value);
    }

    [[nodiscard]] static db_field borrowed_field(std::string_view value) noexcept {
        return db_field::borrowed(value);
    }

    [[nodiscard]] static db_row borrowed_row(const db_field* fields_value, std::size_t size,
        const std::string* column_names, std::size_t column_count) noexcept {
        return db_row(fields_value, size, column_names, column_count);
    }

    static void reserve_result(db_rows& result_value, std::size_t row_count, std::size_t field_count) {
        result_value.rows_.reserve(row_count);
        result_value.fields_.reserve(row_count * field_count);
        result_value.column_names_.reserve(field_count);
    }

    static void append_row(db_rows& result_value, std::size_t row_start, std::size_t field_count) {
        result_value.rows_.push_back(borrowed_row(result_value.fields_.data() + row_start, field_count,
            result_value.column_names_.data(), result_value.column_names_.size()));
    }

    template <typename field_at_type>
    [[nodiscard]] static db_row materialize_stream_row(std::vector<db_field>& fields_value,
        const std::vector<std::string>& column_names, std::size_t field_count,
        field_at_type&& field_at) {
        fields_value.clear();
        fields_value.reserve(field_count);
        for (std::size_t field = 0; field < field_count; ++field) {
            if (const auto value = field_at(field); value.has_value()) {
                fields_value.push_back(owned_field(*value));
            } else {
                fields_value.push_back(null_field());
            }
        }
        return borrowed_row(fields_value.data(), fields_value.size(), column_names.data(), column_names.size());
    }
};

}  // namespace detail

}  // namespace ruvia
