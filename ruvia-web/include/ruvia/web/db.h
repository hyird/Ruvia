#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/model.h"

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

namespace ruvia::detail {

// Walking SQL text well enough to tell code from data.
//
// Two callers need the same distinction and got it wrong in the same way when
// each scanned for a single byte: the MariaDB parameter binder looked for '?'
// and the migration validator looks for a statement separator. A quoted
// literal, a quoted identifier and a comment can all contain either byte, and
// treating those occurrences as syntax silently shifts every parameter after
// them onto the wrong slot.
//
// This is deliberately not a SQL parser. It knows exactly the constructs that
// can hide a byte from the scanner -- '...' and "..." literals, `...`
// identifiers, "--"/"#" line comments and block comments -- and steps over each
// one atomically. Anything else advances a single byte.
//
// MariaDB honours backslash escapes inside '...' and "..." runs unless the
// session runs with NO_BACKSLASH_ESCAPES, where a backslash is data and only a
// doubled quote escapes. The two modes disagree on where a literal ending in a
// backslash closes, and scanning with the wrong one can bind a parameter into
// the middle of a later literal, so the parameter binder passes the mode its
// connection reports. Checks without a connection assume the default mode.

[[nodiscard]] inline constexpr bool is_sql_whitespace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

// MariaDB's lexer peeks at the byte after "--" and sees its terminating NUL at
// the end of the statement, so a final "--" is a comment as well.
[[nodiscard]] inline constexpr bool is_mariadb_double_dash_comment(
    std::string_view sql, std::size_t index) noexcept {
    if (index + 2 > sql.size() || sql[index] != '-' || sql[index + 1] != '-') {
        return false;
    }
    if (index + 2 == sql.size()) {
        return true;
    }
    const auto byte = static_cast<unsigned char>(sql[index + 2]);
    return byte <= 0x20 || byte == 0x7f;
}

[[nodiscard]] inline constexpr bool has_sql_non_whitespace(std::string_view sql) noexcept {
    for (const auto character : sql) {
        if (!is_sql_whitespace(character)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::size_t skip_sql_quoted_run(
    std::string_view sql, std::size_t index, char terminator, bool backslash_escapes) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    auto cursor_value = index + 1;
    while (cursor_value < size) {
        const auto character = sql[cursor_value];
        if (backslash_escapes && character == '\\') {
            cursor_value += 2;
            continue;
        }
        if (character == terminator) {
            // A doubled terminator is one escaped terminator byte, not the end
            // of the run: 'a''b', "a""b" and `a``b` are single tokens.
            if (cursor_value + 1 < size && sql[cursor_value + 1] == terminator) {
                cursor_value += 2;
                continue;
            }
            return cursor_value + 1;
        }
        ++cursor_value;
    }
    return size;
}

[[nodiscard]] constexpr std::size_t skip_sql_line_comment(
    std::string_view sql, std::size_t start, bool carriage_return_ends_comment) noexcept {
    auto cursor_value = start;
    while (cursor_value < sql.size() && sql[cursor_value] != '\n' && (!carriage_return_ends_comment || sql[cursor_value] != '\r')) {
        ++cursor_value;
    }
    return cursor_value < sql.size() ? cursor_value + 1 : sql.size();
}

[[nodiscard]] constexpr std::size_t skip_sql_block_comment(
    std::string_view sql, std::size_t index) noexcept {
    auto cursor_value = index + 2;
    while (cursor_value + 1 < sql.size()) {
        if (sql[cursor_value] == '*' && sql[cursor_value + 1] == '/') {
            return cursor_value + 2;
        }
        ++cursor_value;
    }
    return sql.size();
}

[[nodiscard]] constexpr std::size_t skip_postgresql_block_comment(std::string_view sql, std::size_t index) noexcept {
    std::size_t depth = 1;
    auto cursor_value = index + 2;
    while (cursor_value + 1 < sql.size()) {
        if (sql[cursor_value] == '/' && sql[cursor_value + 1] == '*') {
            ++depth;
            cursor_value += 2;
        } else if (sql[cursor_value] == '*' && sql[cursor_value + 1] == '/') {
            cursor_value += 2;
            if (--depth == 0) {
                return cursor_value;
            }
        } else {
            ++cursor_value;
        }
    }
    return sql.size();
}

// The index just past the construct starting at `index`. For an ordinary byte
// that is `index + 1`; for a literal, identifier or comment it is the first
// index after its terminator, or `sql.size()` when the construct is unclosed.
[[nodiscard]] constexpr std::size_t skip_sql_atom(
    std::string_view sql, std::size_t index, bool backslash_escapes) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    switch (sql[index]) {
        case '\'':
        case '"':
            return skip_sql_quoted_run(sql, index, sql[index], backslash_escapes);
        case '`':
            return skip_sql_quoted_run(sql, index, '`', false);
        case '#':
            // MariaDB's second line-comment introducer. PostgreSQL has no '#'
            // comment and uses skip_postgresql_sql_atom() instead.
            return skip_sql_line_comment(sql, index + 1, false);
        case '-':
            // MySQL/MariaDB accept "--" as a line-comment introducer only when
            // the second dash is followed by whitespace or a control byte.
            // Otherwise expressions such as "balance--1" and placeholders such
            // as "--?" stay statement-level SQL.
            if (is_mariadb_double_dash_comment(sql, index)) {
                return skip_sql_line_comment(sql, index + 2, false);
            }
            return index + 1;
        case '/':
            if (index + 1 < size && sql[index + 1] == '*') {
                return skip_sql_block_comment(sql, index);
            }
            return index + 1;
        default:
            return index + 1;
    }
}

[[nodiscard]] constexpr bool is_postgresql_dollar_tag_start(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_' || byte >= 0x80;
}

[[nodiscard]] constexpr bool is_postgresql_dollar_tag_continue(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return is_postgresql_dollar_tag_start(character) || (byte >= '0' && byte <= '9');
}

// PostgreSQL dollar-quoted strings are opaque SQL data just like ordinary
// string literals, but their delimiter is either $$ or $tag$. Keep this scan
// separate from skip_sql_atom(): MariaDB's '?' binder shares that generic helper
// and must not treat its ordinary '$' identifiers as quoted strings.
[[nodiscard]] constexpr std::size_t skip_postgresql_dollar_quoted_atom(
    std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size || sql[index] != '$') {
        return index < size ? index + 1 : size;
    }

    auto delimiter_end = index + 1;
    if (delimiter_end >= size) {
        return size;
    }
    if (sql[delimiter_end] != '$') {
        if (!is_postgresql_dollar_tag_start(sql[delimiter_end])) {
            return index + 1;
        }
        ++delimiter_end;
        while (delimiter_end < size && is_postgresql_dollar_tag_continue(sql[delimiter_end])) {
            ++delimiter_end;
        }
        if (delimiter_end >= size || sql[delimiter_end] != '$') {
            return index + 1;
        }
    }

    const auto delimiter = sql.substr(index, delimiter_end - index + 1);
    const auto closing = sql.find(delimiter, delimiter_end + 1);
    return closing == std::string_view::npos ? size : closing + delimiter.size();
}

[[nodiscard]] constexpr bool is_postgresql_identifier_continue(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return is_postgresql_dollar_tag_continue(character) || byte == '$';
}

[[nodiscard]] constexpr bool has_postgresql_escape_string_prefix(
    std::string_view sql, std::size_t index) noexcept {
    if (index + 1 >= sql.size() || (sql[index] != 'E' && sql[index] != 'e') ||
        sql[index + 1] != '\'') {
        return false;
    }
    return index == 0 || !is_postgresql_identifier_continue(sql[index - 1]);
}

// PostgreSQL differs from MariaDB in the constructs that can hide a statement
// separator: ordinary strings and quoted identifiers use doubled delimiters,
// not backslash escapes, and '#' is an operator byte rather than a comment
// opener. Escape strings keep the explicit E'...' backslash rule.
[[nodiscard]] constexpr std::size_t skip_postgresql_sql_atom(
    std::string_view sql, std::size_t index) noexcept {
    const auto size = sql.size();
    if (index >= size) {
        return size;
    }

    if (has_postgresql_escape_string_prefix(sql, index)) {
        return skip_sql_quoted_run(sql, index + 1, '\'', true);
    }
    // In PostgreSQL an unquoted identifier may contain '$'. A would-be
    // delimiter immediately after an identifier byte is part of that name,
    // not the start of a dollar-quoted string.
    if (sql[index] == '$' &&
        (index == 0 || !is_postgresql_identifier_continue(sql[index - 1]))) {
        const auto next_value = skip_postgresql_dollar_quoted_atom(sql, index);
        if (next_value != index + 1) {
            return next_value;
        }
    }

    switch (sql[index]) {
        case '\'':
        case '"':
            return skip_sql_quoted_run(sql, index, sql[index], false);
        case '`':
            // PostgreSQL does not use backtick identifiers, but stepping over a
            // MariaDB-style quoted identifier keeps backend-specific migrations
            // from being rejected just because an otherwise opaque identifier
            // contains the wanted byte.
            return skip_sql_quoted_run(sql, index, '`', false);
        case '-':
            if (index + 1 < size && sql[index + 1] == '-') {
                return skip_sql_line_comment(sql, index + 2, true);
            }
            return index + 1;
        case '/':
            if (index + 1 < size && sql[index + 1] == '*') {
                return skip_postgresql_block_comment(sql, index);
            }
            return index + 1;
        default:
            return index + 1;
    }
}

// The index of the next `wanted` byte at statement level at or after `from`, or
// npos. Bytes inside literals, quoted identifiers and comments are data and are
// skipped whole. A byte that opens one of those constructs is that construct
// and is never reported as syntax.
[[nodiscard]] constexpr std::size_t find_sql_syntax_byte(
    std::string_view sql, char wanted, bool backslash_escapes, std::size_t from = 0) noexcept {
    for (auto index = from; index < sql.size();) {
        const auto next_value = skip_sql_atom(sql, index, backslash_escapes);
        if (next_value == index + 1 && sql[index] == wanted) {
            return index;
        }
        index = next_value;
    }
    return std::string_view::npos;
}

// PostgreSQL adds dollar-quoted strings to the generic opaque constructs. This
// variant is used by migration statement validation; the MariaDB parameter
// binder intentionally continues to use find_sql_syntax_byte().
[[nodiscard]] constexpr std::size_t find_postgresql_syntax_byte(
    std::string_view sql, char wanted, std::size_t from = 0) noexcept {
    for (auto index = from; index < sql.size();) {
        const auto next_value = skip_postgresql_sql_atom(sql, index);
        if (next_value == index + 1 && sql[index] == wanted) {
            return index;
        }
        index = next_value;
    }
    return std::string_view::npos;
}

}  // namespace ruvia::detail

namespace ruvia {

class db_transaction;
class db_query;
template <typename entity_type, typename executor_type>
class db_repository;
template <typename entity_type, typename executor_type>
class db_query_builder;
template <typename entity_type, typename executor_type>
class db_write_query_builder;
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif

namespace detail {
class db_client_state;
class db_query_cache_state;
struct db_query_backend;
struct db_query_plan;
class worker_client_registry_view;

// The outer scoped operation owns both the driver task and the mapped result.
// Decoding finishes before db_rows releases its backend storage. Scalar and
// column decoders enforce db_mapped_value and construct owning fields in the
// same operation memory domain; raw mappers transfer db_rows itself.
template <typename result_type, typename mapper_type>
task<result_type> map_db_query(task<db_rows> query, mapper_type mapper) {
    auto rows = co_await std::move(query);
    co_return mapper(std::move(rows));
}

inline std::uint64_t db_count_value(const db_rows& rows) {
    if (rows.size() != 1) {
        throw db_conversion_error(
            db_conversion_error::code_type::unexpected_row_count, "database count did not return exactly one row");
    }
    const auto count = rows.front()["count"].as<std::uint64_t>();
    if (!count) {
        throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "database count column is NULL");
    }
    return *count;
}

template <typename result_type, typename mapper_type>
task<std::pair<result_type, std::uint64_t>> map_db_query_and_count(
    task<std::pair<db_rows, db_rows>> query, mapper_type mapper) {
    auto rows = co_await std::move(query);
    const auto count = db_count_value(rows.second);
    co_return std::pair{mapper(std::move(rows.first)), count};
}

struct db_sql_parameter_count {
    std::size_t count_{0};
    bool valid_{true};
};

// Count MariaDB slots, or PostgreSQL's highest numbered parameter. Repeated
// PostgreSQL references bind the same value; '?' remains a PostgreSQL operator.
// This checks the binding contract, not SQL grammar or schema correctness.
[[nodiscard]] constexpr db_sql_parameter_count count_db_sql_parameters(std::string_view sql, db_driver driver) noexcept {
    db_sql_parameter_count result;
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        return {0, false};
    }
    for (std::size_t index = 0; index < sql.size();) {
        if (sql[index] == '\0') {
            return {0, false};
        }
        const auto next_value =
            driver == db_driver::mariadb ? skip_sql_atom(sql, index, true) : skip_postgresql_sql_atom(sql, index);
        if (next_value != index + 1) {
            index = next_value;
            continue;
        }
        if (driver == db_driver::mariadb) {
            result.count_ += sql[index] == '?';
            ++index;
            continue;
        }
        if (sql[index] == '$' && index + 1 < sql.size() && sql[index + 1] >= '0' && sql[index + 1] <= '9') {
            std::size_t number = 0;
            ++index;
            while (index < sql.size() && sql[index] >= '0' && sql[index] <= '9') {
                const auto digit = static_cast<unsigned>(sql[index++] - '0');
                if (number > ((std::numeric_limits<std::size_t>::max)() - digit) / 10) {
                    return {0, false};
                }
                number = number * 10 + digit;
            }
            if (number == 0) {
                return {0, false};
            }
            if (number > result.count_) {
                result.count_ = number;
            }
            continue;
        }
        // Unquoted identifiers can contain '$'; account$1 is not a parameter.
        if (is_postgresql_dollar_tag_start(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80) {
            do {
                ++index;
            } while (index < sql.size() && (is_postgresql_identifier_continue(sql[index]) || static_cast<unsigned char>(sql[index]) >= 0x80));
        } else {
            ++index;
        }
    }
    return result;
}

template <fixed_string sql, db_driver driver, std::size_t parameter_count>
void validate_db_sql_literal(db_driver actual_driver) {
    static_assert(driver == db_driver::unspecified || driver == db_driver::mariadb || driver == db_driver::postgresql,
        "SQL literal requires a valid database driver");
    static_assert(sql.view().find('\0') == std::string_view::npos, "SQL literal must not contain NUL bytes");
    if constexpr (driver == db_driver::unspecified) {
        constexpr auto mariadb_result = count_db_sql_parameters(sql.view(), db_driver::mariadb);
        constexpr auto postgresql_result = count_db_sql_parameters(sql.view(), db_driver::postgresql);
        constexpr bool has_mariadb = mariadb_result.count_ > 0;
        constexpr bool has_postgresql = postgresql_result.count_ > 0 || !postgresql_result.valid_;
        static_assert(!(has_mariadb && has_postgresql),
            "SQL literal with both '?' and '$n' requires an explicit database driver");
        if constexpr (has_postgresql) {
            static_assert(postgresql_result.valid_, "SQL literal contains an invalid parameter index");
            static_assert(postgresql_result.count_ == parameter_count,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::postgresql) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        } else if constexpr (has_mariadb) {
            static_assert(mariadb_result.valid_, "SQL literal contains an invalid parameter index");
            static_assert(mariadb_result.count_ == parameter_count,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::mariadb) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        } else {
            static_assert(parameter_count == 0,
                "SQL placeholder count does not match the number of arguments");
            if (actual_driver != db_driver::mariadb && actual_driver != db_driver::postgresql) {
                throw std::invalid_argument("SQL literal dialect does not match the database driver");
            }
        }
    } else {
        constexpr auto result_value = count_db_sql_parameters(sql.view(), driver);
        static_assert(result_value.valid_, "SQL literal contains an invalid parameter index");
        static_assert(result_value.count_ == parameter_count,
            "SQL placeholder count does not match the number of arguments");
        if (actual_driver != driver) {
            throw std::invalid_argument("SQL literal dialect does not match the database driver");
        }
    }
}
}  // namespace detail

// Statement and completion operations are lazy. Inputs are owned before the
// entry returns, so cold operations may be prepared while a statement runs.
// Only starting overlapping operations throws std::logic_error; that rejected
// operation does not abort the statement already holding the transaction.
//
// The transaction holds its connection under db_config::transaction_timeout_,
// counted from begin_transaction() holding the connection, including the time
// between operations. Each operation runs under the earlier of its own
// deadline and the remaining transaction time. Once that has passed while no
// operation runs, the connection is closed, so the server rolls back and
// releases the transaction's locks; the next operation (commit() and
// rollback() included) fails with db_error::code_type::timeout and ends the
// transaction.
class db_transaction final {
public:
    db_transaction(const db_transaction&) = delete;
    db_transaction& operator=(const db_transaction&) = delete;
    // Operations borrow the address-stable state owned by this object, so a
    // move transfers the state without invalidating a cold or running frame.
    db_transaction(db_transaction&& other) noexcept;
    db_transaction& operator=(db_transaction&&) = delete;
    ~db_transaction();

    [[nodiscard]] bool active() const noexcept;

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_transaction> repository() &;
    template <typename entity_type>
    db_repository<entity_type, db_transaction> repository() && = delete;

    [[nodiscard]] task<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) &;
    task<db_rows> query(std::string_view, std::span<const db_value> = {}) && = delete;
    task<db_rows> query(std::string_view, std::initializer_list<db_value>) & = delete;
    task<db_rows> query(std::string_view, std::initializer_list<db_value>) && = delete;
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) &;
    task<db_exec_result> execute(
        std::string_view, std::span<const db_value> = {}) && = delete;
    task<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) & = delete;
    task<db_exec_result> execute(
        std::string_view, std::initializer_list<db_value>) && = delete;

    // Bound parameters as ordinary arguments, with the same synchronous cloning
    // and the same temporary-safety as db_session::query()/execute().
    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_rows> query(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    task<db_rows> query(params_type&&...) && = delete;

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_exec_result> execute(params_type&&... params) & {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    task<db_exec_result> execute(params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_rows> query(std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    task<db_rows> query(std::string_view, params_type&&...) && = delete;

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, params_type&&... params) & {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }
    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    task<db_exec_result> execute(std::string_view, params_type&&...) && = delete;

    [[nodiscard]] task<void> commit() &;
    task<void> commit() && = delete;
    [[nodiscard]] task<void> rollback() &;
    task<void> rollback() && = delete;
    [[nodiscard]] task<void> savepoint(std::string_view name) &;
    task<void> savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> rollback_to_savepoint(std::string_view name) &;
    task<void> rollback_to_savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> release_savepoint(std::string_view name) &;
    task<void> release_savepoint(std::string_view) && = delete;
    [[nodiscard]] task<void> notify(std::string_view channel, std::string_view payload = {}) &;
    task<void> notify(std::string_view, std::string_view = {}) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::int64_t key) &;
    task<void> advisory_lock(std::int64_t) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    task<void> advisory_lock(std::int32_t, std::int32_t) && = delete;
    [[nodiscard]] task<void> advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    task<void> advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::int64_t key) &;
    task<bool> try_advisory_lock(std::int64_t) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::int32_t class_id, std::int32_t object_id) &;
    task<bool> try_advisory_lock(std::int32_t, std::int32_t) && = delete;
    [[nodiscard]] task<bool> try_advisory_lock(std::string_view key, std::int64_t seed = 0) &;
    task<bool> try_advisory_lock(std::string_view, std::int64_t = 0) && = delete;
    [[nodiscard]] task<void> declare_cursor(
        std::string_view name, std::string_view sql, std::span<const db_value> params = {}, bool scroll = false) &;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::span<const db_value> = {}, bool = false) && = delete;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) & = delete;
    task<void> declare_cursor(
        std::string_view, std::string_view, std::initializer_list<db_value>, bool = false) && = delete;
    [[nodiscard]] task<db_rows> fetch_cursor(std::string_view name, std::uint64_t count = 1) &;
    task<db_rows> fetch_cursor(std::string_view, std::uint64_t = 1) && = delete;
    [[nodiscard]] task<void> close_cursor(std::string_view name) &;
    task<void> close_cursor(std::string_view) && = delete;

private:
    friend class db_session;
    template <typename, typename>
    friend class db_repository;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;

    [[nodiscard]] db_driver query_driver() const;
    [[nodiscard]] task<db_exec_result> execute_query(const db_query& query);
    [[nodiscard]] task<db_exec_result> execute_prepared(std::string sql);
    [[nodiscard]] task<db_rows> query_task(const db_query& query);
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count);
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(std::move(task_value), std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> query_mapped(const db_query& query, mapper_type mapper) {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(std::move(task_value), std::move(mapper)));
    }
    template <typename driver_type>
    friend class detail::db_pool;

    struct lease_type final {
        lease_type(detail::db_pool_ref_type client, std::size_t slot,
            operation_options options) noexcept;
        void abort() noexcept;

        detail::db_pool_ref_type client_;
        std::size_t slot_;
        operation_options options_;
    };

    using operation_state_type = detail::db_operation_state<lease_type>;
    using operation_guard_type = detail::db_operation_guard<lease_type>;

    using state_owner_type = std::unique_ptr<operation_state_type>;

    db_transaction(detail::db_pool_ref_type client, std::size_t slot,
        operation_options options);
    template <typename result_type, bool exec_statement>
    task<result_type> run_statement(std::string sql, std::vector<db_value> params);
    // `io_metrics` is the request's counter block, or null outside a request.
    template <typename result_type, bool exec_statement>
    static task<result_type> statement_task(std::string sql, std::vector<db_value> params,
        detail::request_io_metrics* io_metrics, operation_guard_type operation);
    template <bool commit_lease>
    static task<void> finish_task(detail::request_io_metrics* io_metrics, operation_guard_type operation);
    // A command naming one identifier: SAVEPOINT, RELEASE, CLOSE and the like.
    task<void> run_identifier_command(std::string_view prefix, std::string_view name);
    void require_postgresql_transaction(std::string_view feature) const;
    task<void> query_void_prepared(std::string_view sql, std::span<const db_value> params);
    task<bool> query_bool_prepared(std::string_view sql, std::span<const db_value> params);
    void reset() noexcept;
    void bind_operation_scope(operation_scope& scope, detail::request_io_metrics* io_metrics) noexcept;
    static void expire_capability(void* target) noexcept;

    [[nodiscard]] detail::db_query_backend query_backend(const lease_type& lease) const;
    template <bool with_count>
    static task<std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>> query_plan_prepared(
        detail::db_query_plan plan, detail::db_query_backend backend, operation_guard_type operation);
    state_owner_type state_;
    detail::request_io_metrics* io_metrics_{nullptr};
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_REDIS
class db_query_result_cache final {
public:
    db_query_result_cache(const db_query_result_cache& other) noexcept;
    db_query_result_cache& operator=(const db_query_result_cache&) = delete;
    // Removes the entries cached under each id passed to cache(), including the
    // get_one/get_exists/get_count results a repository builder derived from it.
    [[nodiscard]] task<void> remove(std::span<const std::string_view> ids) const;
    template <typename... ids_type>
        requires(sizeof...(ids_type) > 0 && (std::convertible_to<ids_type&&, std::string_view> && ...))
    [[nodiscard]] task<void> remove(ids_type&&... ids) const {
        const std::string_view views[]{std::string_view(ids)...};
        return remove(std::span<const std::string_view>(views));
    }
    [[nodiscard]] task<void> clear() const;

private:
    friend class db_session;
    db_query_result_cache(detail::db_query_cache_state& state_value, ::ruvia::operation_scope& scope,
        detail::request_io_metrics* io_metrics, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;
    detail::db_query_cache_state* state_;
    detail::request_io_metrics* io_metrics_;
    operation_options options_;
    scoped_capability_registration registration_;
};
#endif

class db_session final {
public:
    db_session(const db_session& other) noexcept;
    db_session& operator=(const db_session&) = delete;

    // Inherits an empty token; a supplied state replaces user cancellation.
    // Timeout can only tighten. Accumulation requires a caller-owned stop_group.
    [[nodiscard]] db_session with_options(operation_options options) const;
    [[nodiscard]] const worker_ref& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] db_query_result_cache query_result_cache() const;
#endif

    template <typename entity_type>
    [[nodiscard]] db_repository<entity_type, db_session> repository() const;

    [[nodiscard]] task<db_rows> query(std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_rows> query(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_exec_result> execute(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    [[nodiscard]] task<db_stream_result> query_stream(
        std::string_view sql, std::span<const db_value> params = {}) const;
    task<db_stream_result> query_stream(
        std::string_view sql, std::initializer_list<db_value> params) const = delete;
    // Bound parameters as ordinary arguments: query(sql, id, name). Each one is
    // converted to a db_value and cloned into the prepared statement before the
    // call returns -- the span overloads below build the statement synchronously,
    // never inside the returned operation -- so argument temporaries only have to
    // outlive the call itself, not the operation that is awaited afterwards.
    //
    // A caller that already holds a contiguous parameter sequence passes it as a
    // span instead; a span cannot construct a db_value, so it never reaches here.
    // Fixed SQL uses MariaDB placeholders by default; specify postgresql
    // for numbered parameters. Values are owned by the existing operation path.
    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_rows> query(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_exec_result> execute(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return execute(sql.view(), std::forward<params_type>(params)...);
    }

    template <fixed_string sql, db_driver driver = db_driver::unspecified, typename... params_type>
        requires(detail::db_parameter<params_type> && ...)
    [[nodiscard]] task<db_stream_result> query_stream(params_type&&... params) const {
        detail::validate_db_sql_literal<sql, driver, sizeof...(params_type)>(query_driver());
        return query_stream(sql.view(), std::forward<params_type>(params)...);
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_rows> query(std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_exec_result> execute(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return execute(sql, std::span<const db_value>(values));
    }

    template <typename... params_type>
        requires detail::db_parameter_pack<params_type...>
    [[nodiscard]] task<db_stream_result> query_stream(
        std::string_view sql, params_type&&... params) const {
        const db_value values[]{detail::make_immediate_db_parameter(std::forward<params_type>(params))...};
        return query_stream(sql, std::span<const db_value>(values));
    }

    [[nodiscard]] task<db_transaction> begin_transaction(
        db_transaction_options options = {}) const;
    [[nodiscard]] task<void> notify(std::string_view channel, std::string_view payload = {}) const;

private:
    friend class detail::db_registry;
    template <typename, typename>
    friend class db_repository;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;

    [[nodiscard]] db_driver query_driver() const;
    [[nodiscard]] task<db_exec_result> execute_query(const db_query& query) const;
    [[nodiscard]] task<db_exec_result> execute_prepared(std::string sql) const;
    [[nodiscard]] task<db_rows> query_task(const db_query& query) const;
    [[nodiscard]] task<std::pair<db_rows, db_rows>> query_and_count_task(const db_query& query, const db_query& count) const;
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<std::pair<result_type, std::uint64_t>> query_mapped_and_count(const db_query& query, const db_query& count, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_and_count_task(query, count);
        return make_scoped_operation(scope,
            detail::map_db_query_and_count<result_type>(std::move(task_value), std::move(mapper)));
    }
    template <typename result_type, typename mapper_type>
    [[nodiscard]] task<result_type> query_mapped(const db_query& query, mapper_type mapper) const {
        registration_.require_active();
        auto& scope = registration_.scope();
        auto task_value = query_task(query);
        return make_scoped_operation(scope,
            detail::map_db_query<result_type>(std::move(task_value), std::move(mapper)));
    }

    // cache is nullptr when the alias has no query cache; without Redis
    // support it is always nullptr. `io_metrics` is the request's counter
    // block, or null outside a request.
    db_session(detail::db_pool_ref_type client,
        operation_scope& operation_scope, detail::request_io_metrics* io_metrics,
        detail::db_query_cache_state* cache) noexcept;
    [[nodiscard]] detail::db_query_backend query_backend() const;
    static task<db_stream_result> query_stream_prepared(detail::db_pool_ref_type client, std::string sql,
        std::vector<db_value> params,
        operation_scope& operation_scope, detail::request_io_metrics* io_metrics, operation_options options);
    static task<db_transaction> begin_transaction_prepared(detail::db_query_backend backend,
        operation_scope& operation_scope, operation_options operation_options,
        db_transaction_options transaction_options);

    detail::db_query_cache_state* cache_{nullptr};
    detail::db_pool_ref_type client_;
    detail::request_io_metrics* io_metrics_{nullptr};
    operation_options options_;
    friend class detail::worker_client_registry_view;
    [[nodiscard]] db_session bind_caller_scope(::ruvia::operation_scope& scope,
        detail::request_io_metrics* io_metrics, const stop_token& stop_token_value) &&;
    static void expire_capability(void* target) noexcept;
    scoped_capability_registration registration_;
};

#ifdef RUVIA_ENABLE_DATABASE
// One database pool bound to one Ruvia event loop. It is the standalone
// counterpart of context::db(): application-created and attached workers use
// the same worker-local backend without an HTTP application or a special context.
class db_client final {
public:
    db_client(event_loop loop, const db_config& config);
#ifdef RUVIA_ENABLE_REDIS
    // Cache operations borrow an existing Redis capability on the same worker.
    // Connect Redis first and shut this client down before its Redis owner.
    db_client(event_loop loop, const db_config& config, const redis_session& cache_store,
        const db_cache_config& cache_policy);
#endif
    ~db_client();

    db_client(const db_client&) = delete;
    db_client& operator=(const db_client&) = delete;
    db_client(db_client&&) = delete;
    db_client& operator=(db_client&&) = delete;

    // Lazy and worker-affine: start/await it on the bound event loop. Creating
    // more than one connect task is harmless; the first task that starts owns
    // the one allowed connection attempt and later starters fail.
    [[nodiscard]] task<void> connect() &;
    task<void> connect() && = delete;

    // Handles and operations borrow this client's open lifecycle. A temporary
    // client would begin pool shutdown at the end of the full expression.
    // Each call registers one allocation-free capability with the client
    // operation scope, so keep the returned handle for a sequence of commands
    // instead of calling db() per command. Like context::db(), the handle
    // carries no operation options; derive them with with_options().
    [[nodiscard]] db_session db() const&;
    db_session db() const&& = delete;

    // Idempotent and callable from any thread. Pool teardown runs on the bound
    // event loop; use shutdown() when its completion must be awaited.
    void close() noexcept;
    // Pending pool waiters report db_error::code_type::closing, distinct from
    // user-token cancellation (db_error::code_type::cancelled).
    // Requests cancellation, joins worker-owned operations, and completes on
    // the bound event loop after the client teardown is finished.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::db_client_state> state_;
};
#endif

namespace detail {
// Expression and nested-query depth accepted by import, compilation and
// relation paths. Same-operator AND/OR chains do not nest.
inline constexpr std::size_t max_db_query_depth = 256;
class db_query_storage;
struct db_query_node;
enum class db_node_kind : std::uint8_t;
class db_query_compiler;
class db_relation_plan;
struct qualified_expression_importer;
struct db_expr_tree;
class db_owned_expression;
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif
}  // namespace detail

enum class db_parameter_mode : std::uint8_t { bound,
    literal };
enum class db_binary_operator : std::uint8_t {
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    and_value,
    or_value,
    add,
    subtract,
    multiply,
    divide,
    modulo,
    concat,
    like,
    not_like,
    i_like,
    not_i_like,
    in,
    not_in,
    is_distinct_from,
    is_not_distinct_from,
    bit_and,
    bit_or,
    bit_xor,
    json_get,
    json_get_text,
    json_path,
    json_path_text,
    json_contains,
    json_contained_by,
    json_has_key,
    json_has_any_key,
    json_has_all_keys,
    json_concat,
    json_delete,
    json_delete_path,
    array_contains,
    array_contained_by,
    array_overlap,
    regex,
    regex_insensitive,
    inet_contains,
    inet_contains_or_equal,
    inet_contained_by,
    inet_contained_by_or_equal,
    inet_overlap,
};
enum class db_unary_operator : std::uint8_t { not_value,
    negate,
    bit_not,
    is_null,
    is_not_null,
    is_true,
    is_false,
    is_not_true,
    is_not_false };
enum class db_join_type : std::uint8_t { inner,
    left,
    right,
    full,
    cross };
enum class db_order_direction : std::uint8_t { asc,
    desc };
enum class db_nulls_order : std::uint8_t { default_value,
    first,
    last };
enum class db_row_lock : std::uint8_t { update,
    no_key_update,
    share,
    key_share };
enum class db_materialization : std::uint8_t { default_value,
    materialized,
    not_materialized };
enum class db_set_operation : std::uint8_t { union_value,
    union_all,
    intersect,
    intersect_all,
    except,
    except_all };
enum class db_window_frame : std::uint8_t { rows,
    range,
    groups };
enum class db_frame_boundary : std::uint8_t { unbounded_preceding,
    preceding,
    current_row,
    following,
    unbounded_following };
enum class db_date_part : std::uint8_t { epoch,
    year,
    month,
    day,
    hour,
    minute,
    second,
    dow,
    doy,
    week,
    quarter };

struct db_type_definition final {
    db_data_type data_type_{db_data_type::inferred};
    borrowed_text custom_name_{};
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    bool array_{false};
};

// Borrows the address-stable AST owned by its query. Import an expression or
// pass it to an ORM operation while that query is alive; no operation retains
// this view after returning its cold asynchronous operation.
class db_expression final {
public:
    db_expression() noexcept = default;
    [[nodiscard]] bool empty() const noexcept {
        return owner_ == nullptr;
    }

private:
    friend class db_query;
    friend class detail::db_owned_expression;
    friend class detail::db_query_compiler;
    friend class detail::db_relation_plan;
    db_expression(const detail::db_query_storage* owner_value, std::size_t node_value) noexcept
        : owner_(owner_value),
          node_(node_value) {}
    const detail::db_query_storage* owner_{nullptr};
    std::size_t node_{0};
};

struct db_window_boundary final {
    db_frame_boundary kind_{db_frame_boundary::current_row};
    query_row_count offset_{};
};
struct db_window_frame_options final {
    db_window_frame kind_{db_window_frame::rows};
    db_window_boundary start_{db_frame_boundary::unbounded_preceding};
    db_window_boundary end_{db_frame_boundary::current_row};
};
struct db_named_argument final {
    borrowed_text name_{};
    db_expression value_{};
};
struct db_source_column final {
    borrowed_text name_{};
    db_type_definition type_{};
};
// span 及元素中的文本均为借用；调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_source_options final {
    bool lateral_{false};
    bool with_ordinality_{false};
    borrowed_span<db_source_column> columns_{};
};
// span 及元素中的文本均为借用；with 入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_cte_options final {
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    borrowed_span<borrowed_text> columns_{};
};
// span 及元素中的文本均为借用；lock/set_lock 入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_lock_options final {
    db_row_lock mode_{db_row_lock::update};
    bool nowait_{false};
    bool skip_locked_{false};
    borrowed_span<borrowed_text> tables_{};
};

class db_query;
class db_predicate;
class db_expr;
class db_tuple;
struct db_order_term;
struct db_window_options;
struct db_case_branch;
struct db_selection;
struct db_assignment;

namespace detail {

class db_owned_expression final {
public:
    enum class leaf_kind : std::uint8_t {
        empty,
        borrowed,
        null_value,
        default_value,
        star,
        column,
        excluded,
        signed_value,
        unsigned_value,
        double_value,
        bool_value,
        tree,
    };

    db_owned_expression() noexcept;
    db_owned_expression(const db_owned_expression& other);
    db_owned_expression& operator=(const db_owned_expression& other);
    db_owned_expression(db_owned_expression&& other) noexcept;
    db_owned_expression& operator=(db_owned_expression&& other) noexcept;
    ~db_owned_expression();

    [[nodiscard]] bool empty() const noexcept {
        return kind_ == leaf_kind::empty;
    }
    [[nodiscard]] db_expression import_into(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const;

    [[nodiscard]] static db_owned_expression from_borrowed(
        db_expression expression, borrowed_text table = {}, bool aliased = false) noexcept;
    [[nodiscard]] static db_owned_expression from_expression(
        db_expression expression, std::string_view table = {}, bool aliased = false);
    [[nodiscard]] static db_owned_expression make_null() noexcept;
    [[nodiscard]] static db_owned_expression make_default() noexcept;
    [[nodiscard]] static db_owned_expression make_star(std::string_view table = {});
    [[nodiscard]] static db_owned_expression make_column(
        std::string_view name, std::string_view table = {}, bool explicit_alias = false);
    [[nodiscard]] static db_owned_expression make_static_column(
        std::string_view name, std::string_view table, std::string_view alias);
    [[nodiscard]] static db_owned_expression make_excluded(std::string_view name);
    [[nodiscard]] static db_owned_expression make_static_excluded(std::string_view name) noexcept;
    [[nodiscard]] static db_owned_expression make_value(const db_value& value);
    [[nodiscard]] static db_owned_expression make_raw_sql(
        std::span<const std::string_view> parts, std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_call(
        std::string_view function, std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_coalesce(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_null_if(
        const db_owned_expression& lhs, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_greatest(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_least(std::span<const db_expr> args);
    [[nodiscard]] static db_owned_expression make_case_when(
        std::span<const db_case_branch> branches, const db_owned_expression& otherwise);
    [[nodiscard]] static db_owned_expression make_jsonb_set(const db_owned_expression& target,
        std::span<const std::string_view> path, const db_owned_expression& new_value, bool create_if_missing);
    [[nodiscard]] static db_owned_expression make_json_path_op(
        const db_owned_expression& target, db_binary_operator op, std::span<const std::string_view> path);
    [[nodiscard]] static db_owned_expression make_aggregate(std::string_view function,
        std::span<const db_expr> args, bool distinct, std::span<const db_order_term> order);
    [[nodiscard]] static db_owned_expression make_count_star();
    [[nodiscard]] static db_owned_expression make_filter(
        const db_owned_expression& aggregate, const db_owned_expression& predicate);
    [[nodiscard]] static db_owned_expression make_over(
        const db_owned_expression& function, const db_window_options& window);
    [[nodiscard]] static db_owned_expression make_within_group(
        const db_owned_expression& function, std::span<const db_order_term> order);
    [[nodiscard]] static db_owned_expression make_cast(
        const db_owned_expression& operand, const db_type_definition& type);
    [[nodiscard]] static db_owned_expression make_at_time_zone(
        const db_owned_expression& operand, const db_owned_expression& zone);
    [[nodiscard]] static db_owned_expression make_extract(
        db_date_part part, const db_owned_expression& operand);
    [[nodiscard]] static db_owned_expression make_subscript(
        const db_owned_expression& array, const db_owned_expression& index);
    [[nodiscard]] static db_owned_expression make_collate(
        const db_owned_expression& operand, std::string_view collation);
    [[nodiscard]] static db_owned_expression make_binary(
        const db_owned_expression& lhs, db_binary_operator op, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_binary(
        db_owned_expression&& lhs, db_binary_operator op, const db_owned_expression& rhs);
    [[nodiscard]] static db_owned_expression make_unary(
        db_unary_operator op, const db_owned_expression& operand);
    [[nodiscard]] static db_owned_expression make_unary(
        db_unary_operator op, db_owned_expression&& operand);
    [[nodiscard]] static db_owned_expression make_between(const db_owned_expression& operand,
        const db_value& lower, const db_value& upper, bool negate);
    [[nodiscard]] static db_owned_expression make_tuple(std::span<const db_expr> elements);
    [[nodiscard]] static db_owned_expression make_in_list(const db_owned_expression& lhs,
        db_binary_operator op, std::size_t count, const void* context,
        db_value (*element_at)(const void* context, std::size_t index));
    template <typename t_type>
    [[nodiscard]] static db_owned_expression make_in_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const t_type> values) {
        return make_in_list(lhs, op, values.size(), values.data(),
            [](const void* context, std::size_t index) -> db_value {
                return make_immediate_db_parameter(static_cast<const t_type*>(context)[index]);
            });
    }
    [[nodiscard]] static db_owned_expression make_in_expr_list(
        const db_owned_expression& lhs, db_binary_operator op, std::span<const db_tuple> values);
    [[nodiscard]] static db_owned_expression make_in_subquery(
        const db_owned_expression& lhs, db_binary_operator op, const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_exists(const db_query& subquery);
    [[nodiscard]] static db_owned_expression make_array_op(const db_owned_expression& lhs,
        db_binary_operator op, std::size_t count, const void* context,
        db_value (*element_at)(const void* context, std::size_t index),
        const db_type_definition& array_type);
    template <typename t_type>
    [[nodiscard]] static db_owned_expression make_array_op(const db_owned_expression& lhs,
        db_binary_operator op, std::span<const t_type> values, const db_type_definition& array_type) {
        return make_array_op(lhs, op, values.size(), values.data(), [](const void* context, std::size_t index) -> db_value { return make_immediate_db_parameter(static_cast<const t_type*>(context)[index]); }, array_type);
    }

private:
    using tree_owner_type = std::unique_ptr<db_expr_tree>;

    [[nodiscard]] static tree_owner_type allocate_tree();
    [[nodiscard]] static db_owned_expression from_tree(tree_owner_type tree) noexcept;
    [[nodiscard]] static db_owned_expression make_variadic(
        db_node_kind kind, std::span<const db_expr> args, std::string_view function);
    [[nodiscard]] static db_owned_expression make_two_operand_node(
        db_node_kind kind, const db_owned_expression& lhs, const db_owned_expression& rhs);
    [[nodiscard]] std::size_t append_to_tree(db_expr_tree& target) const;
    leaf_kind kind_{leaf_kind::empty};
    bool aliased_{false};
    db_expression borrowed_{};
    std::string_view text_{};
    std::string_view table_{};
    union scalar_union {
        constexpr scalar_union() noexcept
            : signed_value_(0) {}
        std::int64_t signed_value_;
        std::uint64_t unsigned_value_;
        double double_value_;
        bool bool_value_;
    } scalar_{};
    tree_owner_type tree_;
};
}  // namespace detail

// An owning condition. Literal arguments are copied during construction, so a
// condition may outlive the strings used to build it. Moving leaves the source empty.
class db_predicate final {
public:
    db_predicate() noexcept = default;
    db_predicate(detail::db_owned_expression owned) noexcept
        : owned_(std::move(owned)) {}
    db_predicate(db_expression expression) noexcept
        : owned_(detail::db_owned_expression::from_borrowed(expression, {}, true)) {}
    db_predicate(const db_predicate&) = default;
    db_predicate& operator=(const db_predicate&) = default;
    db_predicate(db_predicate&&) noexcept = default;
    db_predicate& operator=(db_predicate&&) noexcept = default;

    [[nodiscard]] static db_predicate from_expression(db_expression expression) {
        return db_predicate(detail::db_owned_expression::from_expression(expression));
    }
    [[nodiscard]] static db_predicate exists(const db_query& subquery) {
        return db_predicate(detail::db_owned_expression::make_exists(subquery));
    }
    [[nodiscard]] static db_predicate not_exists(const db_query& subquery) {
        return !exists(subquery);
    }
    [[nodiscard]] bool empty() const noexcept {
        return owned_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& query, std::string_view table = {}, std::string_view alias = {}) const {
        return owned_.empty() ? db_expression{} : owned_.import_into(query, table, alias);
    }
    db_predicate& and_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            owned_ = other.owned_;
        } else {
            owned_ = detail::db_owned_expression::make_binary(
                std::move(owned_), db_binary_operator::and_value, other.owned_);
        }
        return *this;
    }
    db_predicate& or_where(const db_predicate& other) {
        if (other.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        if (empty()) {
            owned_ = other.owned_;
        } else {
            owned_ = detail::db_owned_expression::make_binary(
                std::move(owned_), db_binary_operator::or_value, other.owned_);
        }
        return *this;
    }
    friend db_predicate operator&&(db_predicate left, const db_predicate& right) {
        return combine(std::move(left), db_binary_operator::and_value, right);
    }
    friend db_predicate operator||(db_predicate left, const db_predicate& right) {
        return combine(std::move(left), db_binary_operator::or_value, right);
    }
    friend db_predicate operator!(db_predicate value) {
        if (value.empty()) {
            throw std::invalid_argument("cannot negate an empty database condition");
        }
        return db_predicate(
            detail::db_owned_expression::make_unary(db_unary_operator::not_value, std::move(value.owned_)));
    }

private:
    friend class db_expr;
    template <typename, fixed_string>
    friend class db_field_reference;
    static db_predicate combine(db_predicate left, db_binary_operator op, const db_predicate& right) {
        if (left.empty() || right.empty()) {
            throw std::invalid_argument("cannot combine empty database conditions");
        }
        return db_predicate(
            detail::db_owned_expression::make_binary(std::move(left.owned_), op, right.owned_));
    }
    detail::db_owned_expression owned_{};
};

class db_expr final {
public:
    db_expr() noexcept = default;
    db_expr(detail::db_owned_expression owned) noexcept
        : owned_(std::move(owned)) {}
    db_expr(db_expression expression, borrowed_text table = {}, bool aliased = false) noexcept
        : owned_(detail::db_owned_expression::from_borrowed(expression, table, aliased)) {}
    db_expr(const db_predicate& predicate)
        : owned_(predicate.owned_) {}
    db_expr(db_predicate&& predicate) noexcept
        : owned_(std::move(predicate.owned_)) {}

    [[nodiscard]] static db_expr from_expression(
        db_expression expression, std::string_view table = {}, bool aliased = false) {
        return db_expr(detail::db_owned_expression::from_expression(expression, table, aliased));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] static db_expr value(v_type&& val) {
        return db_expr(detail::db_owned_expression::make_value(
            detail::make_immediate_db_parameter(std::forward<v_type>(val))));
    }
    [[nodiscard]] static db_expr null() noexcept {
        return db_expr(detail::db_owned_expression::make_null());
    }
    [[nodiscard]] static db_expr default_value() noexcept {
        return db_expr(detail::db_owned_expression::make_default());
    }
    [[nodiscard]] static db_expr star(std::string_view table = {}) {
        return db_expr(detail::db_owned_expression::make_star(table));
    }
    [[nodiscard]] static db_expr column(std::string_view name, std::string_view table = {}) {
        return db_expr(detail::db_owned_expression::make_column(name, table));
    }
    [[nodiscard]] static db_expr excluded(std::string_view name) {
        return db_expr(detail::db_owned_expression::make_excluded(name));
    }
    [[nodiscard]] static db_expr raw_sql(std::string_view expression) {
        return raw_sql(std::span<const std::string_view>(&expression, 1), {});
    }
    [[nodiscard]] static db_expr raw_sql(std::span<const std::string_view> parts, std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_raw_sql(parts, args));
    }
    [[nodiscard]] static db_expr raw_sql(
        std::initializer_list<std::string_view> parts, std::initializer_list<db_expr> args) {
        return raw_sql(std::span<const std::string_view>(parts.begin(), parts.size()),
            std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr now() {
        return call("now");
    }
    [[nodiscard]] static db_expr clock_timestamp() {
        return call("clock_timestamp");
    }
    [[nodiscard]] static db_expr interval(std::string_view spec) {
        return value(spec).cast(db_data_type::interval);
    }
    [[nodiscard]] static db_expr call(std::string_view function, std::span<const db_expr> args = {}) {
        return db_expr(detail::db_owned_expression::make_call(function, args));
    }
    [[nodiscard]] static db_expr call(std::string_view function, std::initializer_list<db_expr> args) {
        return call(function, std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr coalesce(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_coalesce(args));
    }
    [[nodiscard]] static db_expr coalesce(std::initializer_list<db_expr> args) {
        return coalesce(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr null_if(const db_expr& lhs, const db_expr& rhs) {
        return db_expr(detail::db_owned_expression::make_null_if(lhs.owned_, rhs.owned_));
    }
    [[nodiscard]] static db_expr greatest(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_greatest(args));
    }
    [[nodiscard]] static db_expr greatest(std::initializer_list<db_expr> args) {
        return greatest(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static db_expr least(std::span<const db_expr> args) {
        return db_expr(detail::db_owned_expression::make_least(args));
    }
    [[nodiscard]] static db_expr least(std::initializer_list<db_expr> args) {
        return least(std::span<const db_expr>(args.begin(), args.size()));
    }
    [[nodiscard]] static inline db_expr case_when(
        std::span<const db_case_branch> branches, const db_expr& otherwise = {});
    [[nodiscard]] static inline db_expr case_when(
        std::initializer_list<db_case_branch> branches, const db_expr& otherwise = {});

    [[nodiscard]] static db_expr jsonb_build_object(std::span<const db_expr> args) {
        return call("jsonb_build_object", args);
    }
    [[nodiscard]] static db_expr jsonb_build_object(std::initializer_list<db_expr> args) {
        return call("jsonb_build_object", args);
    }
    [[nodiscard]] static db_expr jsonb_build_array(std::span<const db_expr> args) {
        return call("jsonb_build_array", args);
    }
    [[nodiscard]] static db_expr jsonb_build_array(std::initializer_list<db_expr> args) {
        return call("jsonb_build_array", args);
    }
    [[nodiscard]] static db_expr jsonb_set(const db_expr& target, std::span<const std::string_view> path,
        const db_expr& new_value, bool create_if_missing = true) {
        return db_expr(detail::db_owned_expression::make_jsonb_set(
            target.owned_, path, new_value.owned_, create_if_missing));
    }
    [[nodiscard]] static db_expr jsonb_set(const db_expr& target, std::initializer_list<std::string_view> path,
        const db_expr& new_value, bool create_if_missing = true) {
        return jsonb_set(target, std::span<const std::string_view>(path.begin(), path.size()), new_value, create_if_missing);
    }
    [[nodiscard]] static db_expr jsonb_strip_nulls(const db_expr& target) {
        return call("jsonb_strip_nulls", std::span<const db_expr>(&target, 1));
    }

    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::span<const db_expr> args,
        bool distinct = false, std::span<const db_order_term> order = {});
    [[nodiscard]] static inline db_expr aggregate(std::string_view function, std::initializer_list<db_expr> args,
        bool distinct = false, std::initializer_list<db_order_term> order = {});
    [[nodiscard]] static db_expr count() {
        return db_expr(detail::db_owned_expression::make_count_star());
    }
    [[nodiscard]] static inline db_expr count(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr sum(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr min(const db_expr& arg);
    [[nodiscard]] static inline db_expr max(const db_expr& arg);
    [[nodiscard]] static inline db_expr avg(const db_expr& arg, bool distinct = false);
    [[nodiscard]] static inline db_expr bool_and(const db_expr& arg);
    [[nodiscard]] static inline db_expr bool_or(const db_expr& arg);
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::span<const db_order_term> order = {});
    [[nodiscard]] static inline db_expr jsonb_agg(const db_expr& arg, std::initializer_list<db_order_term> order);

    [[nodiscard]] bool empty() const noexcept {
        return owned_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        if (owned_.empty()) {
            throw std::invalid_argument("cannot evaluate an empty database expression");
        }
        return owned_.import_into(target, table, alias);
    }
    [[nodiscard]] inline db_selection as(borrowed_text alias_name) const;

    [[nodiscard]] db_expr cast(const db_type_definition& type) const {
        return db_expr(detail::db_owned_expression::make_cast(owned_, type));
    }
    [[nodiscard]] db_expr cast(db_data_type type) const {
        return cast(db_type_definition{.data_type_ = type});
    }
    [[nodiscard]] db_expr at_time_zone(const db_expr& zone) const {
        return db_expr(detail::db_owned_expression::make_at_time_zone(owned_, zone.owned_));
    }
    [[nodiscard]] db_expr at_time_zone(std::string_view zone) const {
        return at_time_zone(value(zone));
    }
    [[nodiscard]] db_expr extract(db_date_part part) const {
        return db_expr(detail::db_owned_expression::make_extract(part, owned_));
    }
    [[nodiscard]] db_expr subscript(const db_expr& index) const {
        return db_expr(detail::db_owned_expression::make_subscript(owned_, index.owned_));
    }
    [[nodiscard]] db_expr subscript(std::int64_t index) const {
        return subscript(value(index));
    }
    [[nodiscard]] db_expr operator[](std::int64_t index) const {
        return subscript(index);
    }
    [[nodiscard]] db_expr collate(std::string_view collation) const {
        return db_expr(detail::db_owned_expression::make_collate(owned_, collation));
    }
    [[nodiscard]] db_expr concat(const db_expr& rhs) const {
        return binary(db_binary_operator::concat, rhs);
    }
    [[nodiscard]] db_expr concat(std::string_view rhs) const {
        return binary(db_binary_operator::concat, value(rhs));
    }
    [[nodiscard]] db_expr json_get(std::string_view key) const {
        return binary(db_binary_operator::json_get, value(key));
    }
    [[nodiscard]] db_expr json_get(std::int64_t index) const {
        return binary(db_binary_operator::json_get, value(index));
    }
    [[nodiscard]] db_expr json_get_text(std::string_view key) const {
        return binary(db_binary_operator::json_get_text, value(key));
    }
    [[nodiscard]] db_expr json_get_text(std::int64_t index) const {
        return binary(db_binary_operator::json_get_text, value(index));
    }
    [[nodiscard]] db_expr json_path(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_path, path);
    }
    [[nodiscard]] db_expr json_path(std::initializer_list<std::string_view> path) const {
        return json_path(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_path_text(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_path_text, path);
    }
    [[nodiscard]] db_expr json_path_text(std::initializer_list<std::string_view> path) const {
        return json_path_text(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_delete(std::string_view key) const {
        return binary(db_binary_operator::json_delete, value(key));
    }
    [[nodiscard]] db_expr json_delete(std::int64_t index) const {
        return binary(db_binary_operator::json_delete, value(index));
    }
    [[nodiscard]] db_expr json_delete_path(std::span<const std::string_view> path) const {
        return apply_json_path(db_binary_operator::json_delete_path, path);
    }
    [[nodiscard]] db_expr json_delete_path(std::initializer_list<std::string_view> path) const {
        return json_delete_path(std::span<const std::string_view>(path.begin(), path.size()));
    }
    [[nodiscard]] db_expr json_concat(const db_expr& rhs) const {
        return binary(db_binary_operator::json_concat, rhs);
    }
    [[nodiscard]] db_expr filter(const db_predicate& predicate) const {
        if (predicate.empty()) {
            throw std::invalid_argument("aggregate FILTER requires a nonempty condition");
        }
        return db_expr(detail::db_owned_expression::make_filter(owned_, predicate.owned_));
    }
    [[nodiscard]] inline db_expr over(const db_window_options& window) const;
    [[nodiscard]] inline db_expr within_group(std::span<const db_order_term> order) const;
    [[nodiscard]] inline db_expr within_group(std::initializer_list<db_order_term> order) const;
    [[nodiscard]] inline db_order_term asc(db_nulls_order nulls = db_nulls_order::default_value) const;
    [[nodiscard]] inline db_order_term desc(db_nulls_order nulls = db_nulls_order::default_value) const;

    [[nodiscard]] db_expr operator-() const {
        return unary_expr(db_unary_operator::negate);
    }
    [[nodiscard]] db_expr operator~() const {
        return unary_expr(db_unary_operator::bit_not);
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return compare_expr(db_binary_operator::is_not_distinct_from, rhs);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& rhs) const {
        return compare_expr(db_binary_operator::is_distinct_from, value(std::forward<v_type>(rhs)));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& rhs) const {
        return compare_expr(db_binary_operator::is_not_distinct_from, value(std::forward<v_type>(rhs)));
    }
    [[nodiscard]] db_predicate is_null() const {
        return unary_predicate(db_unary_operator::is_null);
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return unary_predicate(db_unary_operator::is_not_null);
    }
    [[nodiscard]] db_predicate is_true() const {
        return unary_predicate(db_unary_operator::is_true);
    }
    [[nodiscard]] db_predicate is_false() const {
        return unary_predicate(db_unary_operator::is_false);
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return unary_predicate(db_unary_operator::is_not_true);
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return unary_predicate(db_unary_operator::is_not_false);
    }
    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return compare_expr(db_binary_operator::like, value(pattern));
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return compare_expr(db_binary_operator::not_like, value(pattern));
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return compare_expr(db_binary_operator::i_like, value(pattern));
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return compare_expr(db_binary_operator::not_i_like, value(pattern));
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return compare_expr(db_binary_operator::regex, value(pattern));
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return compare_expr(db_binary_operator::regex_insensitive, value(pattern));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        return db_predicate(detail::db_owned_expression::make_between(owned_,
            detail::make_immediate_db_parameter(std::forward<lower_type>(lower)),
            detail::make_immediate_db_parameter(std::forward<upper_type>(upper)), false));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return db_predicate(detail::db_owned_expression::make_between(owned_,
            detail::make_immediate_db_parameter(std::forward<lower_type>(lower)),
            detail::make_immediate_db_parameter(std::forward<upper_type>(upper)), true));
    }

    [[nodiscard]] const detail::db_owned_expression& owned() const& noexcept RUVIA_LIFETIMEBOUND {
        return owned_;
    }
    [[nodiscard]] detail::db_owned_expression&& owned() && noexcept {
        return std::move(owned_);
    }

private:
    template <typename, fixed_string>
    friend class db_field_reference;
    [[nodiscard]] db_expr binary(db_binary_operator op, const db_expr& rhs) const {
        return db_expr(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_expr unary_expr(db_unary_operator op) const {
        return db_expr(detail::db_owned_expression::make_unary(op, owned_));
    }
    [[nodiscard]] db_expr apply_json_path(db_binary_operator op, std::span<const std::string_view> path) const {
        return db_expr(detail::db_owned_expression::make_json_path_op(owned_, op, path));
    }
    [[nodiscard]] db_predicate compare_expr(db_binary_operator op, const db_expr& rhs) const {
        return db_predicate(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_predicate unary_predicate(db_unary_operator op) const {
        return db_predicate(detail::db_owned_expression::make_unary(op, owned_));
    }

    detail::db_owned_expression owned_;
};

[[nodiscard]] inline db_expr operator+(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::add, rhs.owned()));
}
[[nodiscard]] inline db_expr operator-(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::subtract, rhs.owned()));
}
[[nodiscard]] inline db_expr operator*(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::multiply, rhs.owned()));
}
[[nodiscard]] inline db_expr operator/(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::divide, rhs.owned()));
}
[[nodiscard]] inline db_expr operator%(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::modulo, rhs.owned()));
}
[[nodiscard]] inline db_expr operator&(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_and, rhs.owned()));
}
[[nodiscard]] inline db_expr operator|(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_or, rhs.owned()));
}
[[nodiscard]] inline db_expr operator^(const db_expr& lhs, const db_expr& rhs) {
    return db_expr(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::bit_xor, rhs.owned()));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator+(const db_expr& lhs, v_type&& rhs) {
    return lhs + db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator-(const db_expr& lhs, v_type&& rhs) {
    return lhs - db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator*(const db_expr& lhs, v_type&& rhs) {
    return lhs * db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator/(const db_expr& lhs, v_type&& rhs) {
    return lhs / db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator%(const db_expr& lhs, v_type&& rhs) {
    return lhs % db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator&(const db_expr& lhs, v_type&& rhs) {
    return lhs & db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator|(const db_expr& lhs, v_type&& rhs) {
    return lhs | db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_expr operator^(const db_expr& lhs, v_type&& rhs) {
    return lhs ^ db_expr::value(std::forward<v_type>(rhs));
}
[[nodiscard]] inline db_predicate operator==(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator!=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::not_equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator<(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::less, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator<=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::less_equal, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator>(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::greater, rhs.owned()));
}
[[nodiscard]] inline db_predicate operator>=(const db_expr& lhs, const db_expr& rhs) {
    return db_predicate(detail::db_owned_expression::make_binary(lhs.owned(), db_binary_operator::greater_equal, rhs.owned()));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator==(const db_expr& lhs, v_type&& rhs) {
    return lhs == db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator!=(const db_expr& lhs, v_type&& rhs) {
    return lhs != db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<(const db_expr& lhs, v_type&& rhs) {
    return lhs < db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator<=(const db_expr& lhs, v_type&& rhs) {
    return lhs <= db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>(const db_expr& lhs, v_type&& rhs) {
    return lhs > db_expr::value(std::forward<v_type>(rhs));
}
template <detail::db_parameter v_type>
[[nodiscard]] db_predicate operator>=(const db_expr& lhs, v_type&& rhs) {
    return lhs >= db_expr::value(std::forward<v_type>(rhs));
}

struct db_order_term final {
    db_expr expression_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
// span 及元素中的文本均为借用；over 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_window_options final {
    borrowed_span<db_expr> partition_by_{};
    borrowed_span<db_order_term> order_by_{};
    std::optional<db_window_frame_options> frame_{};
};
struct db_case_branch final {
    db_expr when_{};
    db_expr then_{};
};

struct db_selection final {
    borrowed_text column_{};
    db_expr expression_{};

    db_selection() = default;
    db_selection(const char* column)
        : column_(column) {}
    db_selection(std::string_view column)
        : column_(column) {}
    template <typename traits_type, typename allocator_type>
    db_selection(std::basic_string<char, traits_type, allocator_type>&&) = delete;
    template <typename traits_type, typename allocator_type>
    db_selection(const std::basic_string<char, traits_type, allocator_type>&&) = delete;
    db_selection(borrowed_text column, db_expr expression)
        : column_(column),
          expression_(std::move(expression)) {}
    // A real SQL wildcard, not a named field. Its qualifier is owned.
    [[nodiscard]] static db_selection star(std::string_view table = {}) {
        db_selection result;
        result.expression_ = db_expr::star(table);
        result.wildcard_ = true;
        return result;
    }
    [[nodiscard]] bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool has_expression() const noexcept {
        return !expression_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return expression_.empty() ? db_expression{} : expression_.expression(target, table, alias);
    }

private:
    bool wildcard_{false};
};

struct db_assignment final {
    borrowed_text column_{};
    db_expr value_{};

    [[nodiscard]] bool empty() const noexcept {
        return value_.empty();
    }
    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return value_.empty() ? db_expression{} : value_.expression(target, table, alias);
    }
};

// span 及元素中的文本均为借用；on_conflict 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_conflict_options final {
    borrowed_span<borrowed_text> columns_{};
    borrowed_text constraint_{};
    db_expression target_where_{};
    borrowed_span<db_assignment> update_{};
    db_expression update_where_{};
    bool do_nothing_{false};
    // MariaDB matches any unique key. This explicit mode does not pretend to
    // implement PostgreSQL's conflict target semantics.
    bool any_unique_key_{false};
};

inline db_expr db_expr::case_when(std::span<const db_case_branch> branches, const db_expr& otherwise) {
    return db_expr(detail::db_owned_expression::make_case_when(branches, otherwise.owned_));
}
inline db_expr db_expr::case_when(std::initializer_list<db_case_branch> branches, const db_expr& otherwise) {
    return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
}
inline db_expr db_expr::aggregate(std::string_view function, std::span<const db_expr> args,
    bool distinct, std::span<const db_order_term> order) {
    return db_expr(detail::db_owned_expression::make_aggregate(function, args, distinct, order));
}
inline db_expr db_expr::aggregate(std::string_view function, std::initializer_list<db_expr> args,
    bool distinct, std::initializer_list<db_order_term> order) {
    return aggregate(function, std::span<const db_expr>(args.begin(), args.size()), distinct,
        std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_expr db_expr::count(const db_expr& arg, bool distinct) {
    return aggregate("count", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::sum(const db_expr& arg, bool distinct) {
    return aggregate("sum", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::min(const db_expr& arg) {
    return aggregate("min", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::max(const db_expr& arg) {
    return aggregate("max", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::avg(const db_expr& arg, bool distinct) {
    return aggregate("avg", std::span<const db_expr>(&arg, 1), distinct);
}
inline db_expr db_expr::bool_and(const db_expr& arg) {
    return aggregate("bool_and", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::bool_or(const db_expr& arg) {
    return aggregate("bool_or", std::span<const db_expr>(&arg, 1));
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::span<const db_order_term> order) {
    return aggregate("jsonb_agg", std::span<const db_expr>(&arg, 1), false, order);
}
inline db_expr db_expr::jsonb_agg(const db_expr& arg, std::initializer_list<db_order_term> order) {
    return jsonb_agg(arg, std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_selection db_expr::as(borrowed_text alias_name) const {
    return db_selection(alias_name, *this);
}
inline db_expr db_expr::over(const db_window_options& window) const {
    return db_expr(detail::db_owned_expression::make_over(owned_, window));
}
inline db_expr db_expr::within_group(std::span<const db_order_term> order) const {
    return db_expr(detail::db_owned_expression::make_within_group(owned_, order));
}
inline db_expr db_expr::within_group(std::initializer_list<db_order_term> order) const {
    return within_group(std::span<const db_order_term>(order.begin(), order.size()));
}
inline db_order_term db_expr::asc(db_nulls_order nulls) const {
    return db_order_term{*this, db_order_direction::asc, nulls};
}
inline db_order_term db_expr::desc(db_nulls_order nulls) const {
    return db_order_term{*this, db_order_direction::desc, nulls};
}

class db_statement final {
public:
    db_statement(const db_statement&) = delete;
    db_statement& operator=(const db_statement&) = delete;
    db_statement(db_statement&&) noexcept = default;
    db_statement& operator=(db_statement&& other);
    [[nodiscard]] std::string_view sql() const& noexcept RUVIA_LIFETIMEBOUND {
        return sql_;
    }
    std::string_view sql() const&& = delete;
    [[nodiscard]] std::span<const db_value> params() const& noexcept RUVIA_LIFETIMEBOUND {
        return params_;
    }
    std::span<const db_value> params() const&& = delete;
    [[nodiscard]] bool returns_rows() const noexcept {
        return returns_rows_;
    }

private:
    friend class db_query;
    friend class db_session;
    friend class db_transaction;
    friend struct detail::db_query_plan;
    template <typename, typename>
    friend class db_query_builder;
    db_statement(std::string sql, std::vector<db_value> params, bool rows)
        : sql_(std::move(sql)),
          params_(std::move(params)),
          returns_rows_(rows) {}
    std::string sql_;
    std::vector<db_value> params_;
    bool returns_rows_{false};
};

// A synchronous relational query AST and expression owner. Values, identifiers
// and nested queries become owned data at each construction call. Expression
// arguments must belong to this owner; import_expression() copies foreign nodes.
// Returned expression views borrow this owner's storage; its memory resource must
// outlive it. Repository and builder entry points copy expression nodes before
// returning, so their source owner can then be destroyed. Compilation is the sole
// place that emits SQL and assigns parameter positions.

class db_query final {
public:
    db_query();
    db_query(const db_query&) = delete;
    db_query& operator=(const db_query&) = delete;
    db_query(db_query&&) noexcept;
    db_query& operator=(db_query&&) noexcept;
    ~db_query();

    [[nodiscard]] db_query clone() const;
    [[nodiscard]] bool returns_rows() const;
    [[nodiscard]] bool has_where() const;
    // Classifies the current statement and its active CTE/set-operation graph,
    // never snapshots retained after a source replacement or failed append.
    [[nodiscard]] bool has_writes() const;
    [[nodiscard]] bool has_grouping() const;
    db_query& cache(const db_cache_setting_type& setting);
    db_query& cache(std::string_view id, std::optional<std::chrono::milliseconds> duration = {});

    // Trusted SQL syntax interleaved with expression arguments. Values belong in
    // value() arguments, never in the syntax strings. parts.size() == args.size()+1.
    db_expression sql(std::span<const std::string_view> parts, std::span<const db_expression> args);
    db_expression sql(std::initializer_list<std::string_view> parts, std::initializer_list<db_expression> args) {
        return sql(std::span<const std::string_view>(parts.begin(), parts.size()), std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression sql(std::string_view expression) {
        return sql(std::span<const std::string_view>(&expression, 1), {});
    }
    db_expression column(std::string_view name, std::string_view table = {});
    db_expression star(std::string_view table = {});
    // Owns the value in this query's resource before returning. Compiled for
    // PostgreSQL, a bound value is untyped wherever the SQL around it gives it a
    // type: the other operand, the target column, the selected function or
    // operator overload, a Boolean context (AND/OR/NOT, IS TRUE...), an
    // explicit cast(). Where PostgreSQL cannot type it ("any"/polymorphic
    // arguments such as json_build_object, concat or array_agg; arithmetic with
    // two bound operands; unary minus or ~; IS [NOT] NULL; GREATEST and friends
    // over bound values only) it carries its C++ category: signed integers
    // $n::int8, unsigned integers $n::numeric (the full uint64 range), doubles
    // ::float8, booleans ::bool, text ::text. Integers in int4-only built-in
    // arguments (left(), round(x, n), jsonb -> n) render as $n::int4.
    db_expression value(const db_value& value);
    template <detail::db_parameter value_type>
        requires(!std::same_as<std::remove_cvref_t<value_type>, db_value>)
    db_expression value(value_type&& value) {
        return this->value(detail::make_immediate_db_parameter(std::forward<value_type>(value)));
    }
    db_expression excluded(std::string_view column);
    db_expression null_value();
    db_expression default_value();
    db_expression call(std::string_view function, std::span<const db_expression> args = {}, std::span<const db_named_argument> named = {});
    db_expression call(std::string_view function, std::initializer_list<db_expression> args) {
        return call(function, std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression coalesce(std::span<const db_expression> args);
    db_expression coalesce(std::initializer_list<db_expression> args) {
        return coalesce(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression null_if(db_expression value, db_expression other);
    db_expression greatest(std::span<const db_expression> args);
    db_expression greatest(std::initializer_list<db_expression> args) {
        return greatest(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression least(std::span<const db_expression> args);
    db_expression least(std::initializer_list<db_expression> args) {
        return least(std::span<const db_expression>(args.begin(), args.size()));
    }
    db_expression binary(db_expression lhs, db_binary_operator op, db_expression rhs);
    db_expression unary(db_unary_operator op, db_expression value);
    db_expression between(db_expression value, db_expression lower, db_expression upper, bool negate = false);
    db_expression tuple(std::span<const db_expression> values);
    db_expression tuple(std::initializer_list<db_expression> values) {
        return tuple(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression list(std::span<const db_expression> values);
    db_expression list(std::initializer_list<db_expression> values) {
        return list(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression array(std::span<const db_expression> values);
    db_expression array(std::initializer_list<db_expression> values) {
        return array(std::span<const db_expression>(values.begin(), values.size()));
    }
    db_expression any(db_expression array);
    db_expression all(db_expression array);
    db_expression cast(db_expression value, const db_type_definition& type);
    db_expression cast(db_expression value, db_data_type type) {
        return cast(value, db_type_definition{.data_type_ = type});
    }
    db_expression alias(db_expression value, std::string_view name);
    db_expression case_when(std::span<const db_case_branch> branches, db_expression otherwise = {});
    db_expression case_when(std::initializer_list<db_case_branch> branches, db_expression otherwise = {}) {
        return case_when(std::span<const db_case_branch>(branches.begin(), branches.size()), otherwise);
    }
    // The const& composition entry points (exists, subquery, from, join,
    // insert_from, with, combine) copy their argument into this query. The
    // rvalue overloads take over an argument already allocated from this
    // query's resource and copy any other; either way the argument is left
    // moved-from unless it is this query itself.
    db_expression exists(const db_query& query);
    db_expression exists(db_query&& query);
    db_expression subquery(const db_query& query);
    db_expression subquery(db_query&& query);
    db_expression aggregate(std::string_view function, std::span<const db_expression> args, bool distinct = false, std::span<const db_order_term> order = {});
    db_expression aggregate(std::string_view function, std::initializer_list<db_expression> args, bool distinct = false, std::span<const db_order_term> order = {}) {
        return aggregate(function, std::span<const db_expression>(args.begin(), args.size()), distinct, order);
    }
    db_expression filter(db_expression aggregate, db_expression predicate);
    db_expression over(db_expression function, const db_window_options& window);
    db_expression within_group(db_expression function, std::span<const db_order_term> order);
    db_expression extract(db_date_part part, db_expression value);
    db_expression subscript(db_expression array, db_expression index);
    db_expression collate(db_expression value, std::string_view collation);
    db_expression at_time_zone(db_expression value, db_expression zone);
    db_expression at_time_zone(db_expression value, std::string_view zone) {
        return at_time_zone(value, this->value(zone));
    }
    // Copies the expression and its referenced subqueries into this query's resource;
    // the source must remain alive during import, but is unchanged and need not outlive the result.
    // Both qualifiers must be empty or valid nonempty names. A mapping rewrites exact
    // column/star qualifiers, including correlated references in nested queries, but not
    // references bound by a local alias or unaliased table name (qualified or unqualified).
    // Raw SQL text is not rewritten. Throws std::invalid_argument for an empty expression,
    // invalid mapping, or a mapped correlated reference captured by a local target binding.
    // Throws std::length_error if expression/query nesting exceeds 256 levels;
    // a chain of one AND or OR operator counts as a single level.
    db_expression import_expression(db_expression expression, std::string_view source_qualifier = {}, std::string_view target_qualifier = {});
    // Renders one expression with inline literals, never bound parameters.
    [[nodiscard]] static std::string render_expression(db_expression expression, db_driver driver);

    // Replacing SELECT, GROUP BY, DISTINCT ON or RETURNING lists preserves
    // the previous clause on validation or allocation failure.
    db_query& select(std::span<const db_expression> projections);
    db_query& select(std::initializer_list<db_expression> projections) {
        return select(std::span<const db_expression>(projections.begin(), projections.size()));
    }
    db_query& select(db_expression projection) {
        return select(std::span<const db_expression>(&projection, 1));
    }
    db_query& add_select(db_expression projection);
    // FROM, JOIN, UPDATE/DELETE/INSERT target aliases and CTE names are single
    // unqualified identifiers: a dotted name could never be referenced.
    db_query& from(std::string_view table, std::string_view alias = {});
    db_query& from(const db_query& query, std::string_view alias, const db_source_options& options = {});
    db_query& from(db_query&& query, std::string_view alias, const db_source_options& options = {});
    db_query& from_function(db_expression function, std::string_view alias, const db_source_options& options = {});
    db_query& clear_source();
    db_query& join(db_join_type type, std::string_view table, db_expression on = {}, std::string_view alias = {}, std::span<const std::string_view> using_columns = {});
    db_query& join(db_join_type type, std::string_view table_value, db_expression on, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join(type, table_value, on, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query& join(db_join_type type, const db_query& query, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& join(db_join_type type, db_query&& query, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& join_function(db_join_type type, db_expression function, db_expression on, std::string_view alias, const db_source_options& options = {});
    db_query& where(db_expression predicate);
    db_query& and_where(db_expression predicate);
    db_query& or_where(db_expression predicate);
    db_query& clear_where();
    db_query& group_by(std::span<const db_expression> expressions);
    db_query& group_by(std::initializer_list<db_expression> expressions) {
        return group_by(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& add_group_by(db_expression expression);
    db_query& having(db_expression predicate);
    db_query& and_having(db_expression predicate);
    db_query& or_having(db_expression predicate);
    db_query& clear_having();
    db_query& order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& add_order_by(db_expression expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value);
    db_query& clear_order();
    db_query& limit(std::optional<query_row_count> count);
    template <std::integral integer_type>
        requires(!std::same_as<integer_type, bool>)
    db_query& limit(integer_type count) {
        if (!std::cmp_greater_equal(+count, 0)) {
            throw std::invalid_argument("database query limit must not be negative");
        }
        return set_limit_value(static_cast<std::uint64_t>(count));
    }
    db_query& offset(std::optional<query_row_count> count);
    template <std::integral integer_type>
        requires(!std::same_as<integer_type, bool>)
    db_query& offset(integer_type count) {
        if (!std::cmp_greater_equal(+count, 0)) {
            throw std::invalid_argument("database query offset must not be negative");
        }
        return set_offset_value(static_cast<std::uint64_t>(count));
    }
    db_query& distinct(bool enabled = true);
    db_query& distinct_on(std::span<const db_expression> expressions);
    db_query& distinct_on(std::initializer_list<db_expression> expressions) {
        return distinct_on(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    db_query& returning(std::span<const db_expression> expressions);
    db_query& returning(std::initializer_list<db_expression> expressions) {
        return returning(std::span<const db_expression>(expressions.begin(), expressions.size()));
    }
    // Existing positional rows cannot be reinterpreted with a different column
    // list. Use insert_rows() to append named rows and grow the column union.
    // insert_into(), update() and delete_from() switch the statement kind only
    // while the query holds no clause beyond WHERE and WITH: a retained FROM
    // would otherwise widen the target (use update_from / delete_using).
    db_query& insert_into(std::string_view table, std::span<const std::string_view> columns = {}, std::string_view alias = {});
    db_query& insert_into(std::string_view table_value, std::initializer_list<std::string_view> columns, std::string_view alias = {}) {
        return insert_into(table_value, std::span<const std::string_view>(columns.begin(), columns.size()), alias);
    }
    db_query& values(std::span<const db_expression> row);
    db_query& values(std::initializer_list<db_expression> row) {
        return values(std::span<const db_expression>(row.begin(), row.size()));
    }
    // Appends a row-major batch to an INSERT, matching fields by column name.
    // Missing fields in old/new rows become DEFAULT. values.size() must equal
    // columns.size() * row_count and every expression must belong to this query.
    // One explicit default-only row is supported; multiple all-default rows
    // require at least one named column. A rejected batch preserves SQL semantics.
    db_query& insert_rows(std::span<const std::string_view> columns,
        std::span<const db_expression> values, std::size_t row_count);
    db_query& insert_from(const db_query& query);
    db_query& insert_from(db_query&& query);
    db_query& on_conflict(const db_conflict_options& conflict);
    db_query& update(std::string_view table, std::string_view alias = {});
    db_query& set(std::string_view column, db_expression value);
    db_query& set(const db_assignment& assignment);
    // A batch is published only after every assignment is prepared. Failure
    // preserves the previous assignments; duplicate columns use the last value.
    db_query& set(std::span<const db_assignment> assignments);
    db_query& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_query& update_from(std::string_view table, std::string_view alias = {});
    db_query& update_from(const db_query& query, std::string_view alias);
    db_query& delete_from(std::string_view table, std::string_view alias = {});
    db_query& delete_using(std::string_view table, std::string_view alias = {});
    db_query& delete_using(const db_query& query, std::string_view alias);
    // Lock target names are the unqualified FROM names or aliases they lock.
    db_query& lock(const db_lock_options& options);
    db_query& clear_lock();
    db_query& with(std::string_view name, const db_query& query, const db_cte_options& options = {});
    db_query& with(std::string_view name, db_query&& query, const db_cte_options& options = {});
    db_query& combine(db_set_operation operation, const db_query& query);
    db_query& combine(db_set_operation operation, db_query&& query);

    // PostgreSQL bound mode numbers each parameter node once and rejects a
    // statement needing more than 65535 parameters (the protocol's Int16
    // count) with std::length_error. In bound mode, raw SQL text must not
    // contain PostgreSQL positional parameters ($1); pass value() arguments.
    [[nodiscard]] db_statement compile(db_driver driver,
        db_parameter_mode mode = db_parameter_mode::bound) const;

private:
    friend class detail::db_owned_expression;
    friend class detail::db_query_compiler;
    friend class detail::db_relation_plan;
    friend struct detail::qualified_expression_importer;
    friend class db_schema;
#ifdef RUVIA_ENABLE_REDIS
    // The result cache reads each statement's cache options.
    friend class detail::db_query_cache_state;
    friend struct detail::db_query_plan;
#endif
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    template <typename, typename>
    friend class db_repository;

    [[nodiscard]] bool cacheable() const;
    [[nodiscard]] std::optional<bool> cache_enabled() const;
    [[nodiscard]] std::optional<std::chrono::milliseconds> cache_duration() const;
    [[nodiscard]] std::string_view cache_id() const;
    [[nodiscard]] detail::db_cache_shape cache_shape() const;
    void copy_cache(const db_query& source, detail::db_cache_shape shape);
    void with_columns(std::string_view name, const db_query& query, db_materialization materialization,
        std::span<const std::string> columns);

    struct storage_deleter_type final {
        void operator()(detail::db_query_storage* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<detail::db_query_storage, storage_deleter_type>;
    explicit db_query(storage_owner_type storage) noexcept;
    [[nodiscard]] static storage_owner_type copy_storage(const detail::db_query_storage& source);
    [[nodiscard]] detail::db_query_storage& storage();
    [[nodiscard]] const detail::db_query_storage& storage() const;
    [[nodiscard]] std::size_t require_expression(db_expression expression) const;
    void require_select_query() const;
    db_query& replace_expressions(std::vector<std::size_t>& target, std::span<const db_expression> expressions);
    db_query& combine_clause_predicate(std::size_t& clause, db_binary_operator op, db_expression predicate);
    // Every expression builder constructs exactly its own node kind and
    // appends it here; the returned handle addresses the appended node.
    [[nodiscard]] detail::db_query_node make_node(detail::db_node_kind kind, std::span<const db_expression> args = {}) const;
    db_expression push_node(detail::db_query_node&& node);
    // Callers prepare clause metadata and reserve every publication slot first.
    [[nodiscard]] db_query adopt(db_query&& query) const;
    [[nodiscard]] db_expression add_subquery(detail::db_node_kind kind, db_query&& query);
    db_query& set_limit_value(std::uint64_t count);
    db_query& set_offset_value(std::uint64_t count);
    storage_owner_type storage_;
};

class db_tuple final {
public:
    db_tuple(std::span<const db_expr> elements)
        : owned_(detail::db_owned_expression::make_tuple(elements)) {}
    db_tuple(std::initializer_list<db_expr> elements)
        : db_tuple(std::span<const db_expr>(elements.begin(), elements.size())) {}

    [[nodiscard]] db_expression expression(
        db_query& target, std::string_view table = {}, std::string_view alias = {}) const {
        return owned_.import_into(target, table, alias);
    }

    [[nodiscard]] db_predicate operator==(const db_tuple& rhs) const {
        return compare(db_binary_operator::equal, rhs);
    }
    [[nodiscard]] db_predicate operator!=(const db_tuple& rhs) const {
        return compare(db_binary_operator::not_equal, rhs);
    }
    [[nodiscard]] db_predicate operator<(const db_tuple& rhs) const {
        return compare(db_binary_operator::less, rhs);
    }
    [[nodiscard]] db_predicate operator<=(const db_tuple& rhs) const {
        return compare(db_binary_operator::less_equal, rhs);
    }
    [[nodiscard]] db_predicate operator>(const db_tuple& rhs) const {
        return compare(db_binary_operator::greater, rhs);
    }
    [[nodiscard]] db_predicate operator>=(const db_tuple& rhs) const {
        return compare(db_binary_operator::greater_equal, rhs);
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_tuple& rhs) const {
        return compare(db_binary_operator::is_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_tuple& rhs) const {
        return compare(db_binary_operator::is_not_distinct_from, rhs);
    }
    [[nodiscard]] db_predicate in(std::span<const db_tuple> rows) const {
        return compare_list(db_binary_operator::in, rows);
    }
    [[nodiscard]] db_predicate in(std::initializer_list<db_tuple> rows) const {
        return in(std::span<const db_tuple>(rows.begin(), rows.size()));
    }
    [[nodiscard]] db_predicate not_in(std::span<const db_tuple> rows) const {
        return compare_list(db_binary_operator::not_in, rows);
    }
    [[nodiscard]] db_predicate not_in(std::initializer_list<db_tuple> rows) const {
        return not_in(std::span<const db_tuple>(rows.begin(), rows.size()));
    }

private:
    friend class detail::db_owned_expression;

    [[nodiscard]] db_predicate compare(db_binary_operator op, const db_tuple& rhs) const {
        return db_predicate(detail::db_owned_expression::make_binary(owned_, op, rhs.owned_));
    }
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const db_tuple> rows) const {
        return db_predicate(detail::db_owned_expression::make_in_expr_list(owned_, op, rows));
    }
    detail::db_owned_expression owned_{};
};

namespace detail {
template <typename e_type>
struct db_entity_access;
}  // namespace detail

enum class db_relation_kind : unsigned char { one_to_one,
    many_to_one,
    one_to_many,
    many_to_many };

template <fixed_string local_name, fixed_string referenced_name>
struct db_join_column final {
    static constexpr auto local = local_name;
    static constexpr auto referenced = referenced_name;
};

template <typename... j_type>
struct db_join_columns final {
    using tuple_type = std::tuple<j_type...>;
};
template <fixed_string field_name>
struct db_inverse final {
    static constexpr auto name = field_name;
};
namespace detail {
template <typename t_type>
struct is_db_inverse : std::false_type {};
template <fixed_string n>
struct is_db_inverse<db_inverse<n>> : std::true_type {};
template <typename t_type>
struct db_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <fixed_string n>
struct db_inverse_name<db_inverse<n>> {
    static constexpr auto value = n;
};
template <typename... t_type>
struct relation_join_tuple {
    using type = std::tuple<t_type...>;
};
template <typename t_type>
    requires requires { typename t_type::tuple_type; }
struct relation_join_tuple<t_type> {
    using type = typename t_type::tuple_type;
};
template <typename... t_type>
struct first_inverse_name {
    static constexpr auto value = fixed_string{""};
};
template <typename t_type, typename... rest_type>
struct first_inverse_name<t_type, rest_type...> {
    static constexpr auto value = [] {
        if constexpr (is_db_inverse<t_type>::value) {
            return db_inverse_name<t_type>::value;
        } else {
            return first_inverse_name<rest_type...>::value;
        }
    }();
};
}  // namespace detail
template <fixed_string table, typename owner, typename inverse_type>
struct db_join_table final {
    static constexpr auto name = table;
    using owner_columns_type = typename owner::tuple_type;
    using inverse_columns_type = typename inverse_type::tuple_type;
};
namespace detail {
template <typename t_type>
struct is_db_join_table : std::false_type {};
template <fixed_string n, typename o_type, typename i_type>
struct is_db_join_table<db_join_table<n, o_type, i_type>> : std::true_type {};
}  // namespace detail

template <fixed_string field_name, typename target_type, typename... js_type>
struct db_many_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_columns_type = std::tuple<js_type...>;
    static constexpr auto kind = db_relation_kind::many_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = true;
    static constexpr auto inverse_name = fixed_string{""};
};
template <fixed_string field_name, typename target_type, typename... mapping_type>
struct db_one_to_one final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_one;
    static constexpr bool is_collection = false;
    static constexpr bool is_owning = !(sizeof...(mapping_type) == 1 && (detail::is_db_inverse<mapping_type>::value && ...));
    using join_columns_type = typename detail::relation_join_tuple<mapping_type...>::type;
    static constexpr auto inverse_name = detail::first_inverse_name<mapping_type...>::value;
};
template <fixed_string field_name, typename target_type, fixed_string inverse>
struct db_one_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    static constexpr auto kind = db_relation_kind::one_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = false;
    static constexpr auto inverse_name = inverse;
    using join_columns_type = std::tuple<>;
};
template <fixed_string field_name, typename target_type, typename mapping_type>
struct db_many_to_many final {
    static constexpr auto name = field_name;
    using target_entity_type = target_type;
    using join_table_type = mapping_type;
    static constexpr auto kind = db_relation_kind::many_to_many;
    static constexpr bool is_collection = true;
    static constexpr bool is_owning = detail::is_db_join_table<mapping_type>::value;
    static constexpr auto inverse_name = [] {
        if constexpr (detail::is_db_inverse<mapping_type>::value) {
            return mapping_type::name;
        } else {
            return fixed_string{""};
        }
    }();
    using join_columns_type = std::tuple<>;
};

enum class db_generated_type : unsigned char { none,
    stored,
    virtual_value };

template <typename enum_name_type = fixed_string<1>, typename default_expression_type = fixed_string<1>>
struct db_column_options final {
    db_data_type data_type_{db_data_type::inferred};
    bool primary_key_{false};
    bool generated_{false};
    db_generated_type generated_type_{db_generated_type::none};
    bool nullable_{false};
    bool read_only_{false};
    bool create_date_{false};
    bool update_date_{false};
    bool delete_date_{false};
    bool version_{false};
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    enum_name_type enum_name_{""};
    default_expression_type default_expression_{""};
    constexpr bool operator==(const db_column_options&) const = default;
};

namespace detail {

template <typename t_type>
struct db_entity_type_traits {
    using value_type = t_type;
    static constexpr db_data_type data_type = db_data_type::inferred;
};
template <>
struct db_entity_type_traits<bool> {
    using value_type = bool;
    static constexpr db_data_type data_type = db_data_type::boolean;
};
template <>
struct db_entity_type_traits<std::int16_t> {
    using value_type = std::int16_t;
    static constexpr db_data_type data_type = db_data_type::small_int;
};
template <>
struct db_entity_type_traits<std::int32_t> {
    using value_type = std::int32_t;
    static constexpr db_data_type data_type = db_data_type::integer;
};
template <>
struct db_entity_type_traits<std::int64_t> {
    using value_type = std::int64_t;
    static constexpr db_data_type data_type = db_data_type::big_int;
};
template <>
struct db_entity_type_traits<float> {
    using value_type = float;
    static constexpr db_data_type data_type = db_data_type::real;
};
template <>
struct db_entity_type_traits<double> {
    using value_type = double;
    static constexpr db_data_type data_type = db_data_type::double_value;
};
template <>
struct db_entity_type_traits<std::string> {
    using value_type = std::string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <>
struct db_entity_type_traits<string> {
    using value_type = string;
    static constexpr db_data_type data_type = db_data_type::text;
};
template <typename t_type>
struct db_entity_type_traits<std::optional<t_type>> {
    using value_type = typename db_entity_type_traits<t_type>::value_type;
    static constexpr db_data_type data_type = db_entity_type_traits<t_type>::data_type;
};
template <typename t_type>
struct db_entity_type_traits<std::vector<t_type>> {
    using value_type = std::vector<t_type>;
    static constexpr db_data_type data_type = db_data_type::array;
};

// Column descriptor predicates shared by typed field references and the
// repository builders so both agree on which columns accept writes and
// arithmetic updates.
template <typename c_type>
consteval bool is_writable_db_column() noexcept {
    return c_type::options.generated_type_ == db_generated_type::none && !c_type::options.read_only_;
}
// Columns a conflict update writes from EXCLUDED when the caller names none.
template <typename c_type>
consteval bool is_conflict_updatable_db_column() noexcept {
    return !c_type::options.primary_key_ && !c_type::options.generated_ && is_writable_db_column<c_type>();
}
template <typename c_type>
consteval bool is_numeric_db_column() noexcept {
    return !is_std_vector<typename c_type::value_type>::value &&
           (c_type::data_type == db_data_type::small_int ||
               c_type::data_type == db_data_type::integer ||
               c_type::data_type == db_data_type::big_int ||
               c_type::data_type == db_data_type::numeric ||
               c_type::data_type == db_data_type::real ||
               c_type::data_type == db_data_type::double_value);
}
consteval bool is_integer_db_data_type(db_data_type type) noexcept {
    return type == db_data_type::small_int || type == db_data_type::integer || type == db_data_type::big_int;
}

// Every rule here depends only on the descriptor, so a malformed column is
// rejected where it is declared instead of when a schema compiles it; the
// repository relies on these invariants (for example, the soft-delete filter
// requires a writable delete_date_ column).
template <typename t_type>
consteval bool valid_db_column_options(const auto& options) noexcept {
    const auto data_type = options.data_type_ == db_data_type::inferred ? db_entity_type_traits<t_type>::data_type : options.data_type_;
    const bool integer = !is_std_vector<t_type>::value && is_integer_db_data_type(data_type);
    const bool computed = options.generated_type_ != db_generated_type::none;
    const bool writable = !computed && !options.read_only_;
    const int lifecycle_flags = int{options.create_date_} + int{options.update_date_} + int{options.delete_date_} + int{options.version_};
    // create_date_ only selects a schema default; the repository writes the other three.
    const bool repository_stamped = options.update_date_ || options.delete_date_ || options.version_;
    return !(options.generated_ && computed) &&
           !(options.read_only_ && (options.generated_ || computed)) &&
           (options.enum_name_.view().empty() || options.data_type_ == db_data_type::inferred || options.data_type_ == db_data_type::array) &&
           (!options.generated_ || (integer && !options.nullable_)) &&
           (!options.version_ || (integer && !options.nullable_)) &&
           (!options.primary_key_ || (!options.nullable_ && !options.delete_date_)) &&
           lifecycle_flags <= 1 &&
           (!repository_stamped || writable);
}

// Table names may be schema-qualified; generated aliases and constraint names
// use the unqualified table name.
constexpr std::string_view unqualified_db_name(std::string_view name) noexcept {
    const auto dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(dot + 1);
}

template <typename t_type, bool collection = false>
struct db_relation_slot final {
    enum class state_type : unsigned char { unset,
        null,
        value };
    using stored_type = std::conditional_t<collection, entity_rows<t_type>, t_type>;
    using pointer = std::unique_ptr<stored_type>;
    db_relation_slot() noexcept = default;
    db_relation_slot(const db_relation_slot&) = delete;
    db_relation_slot& operator=(const db_relation_slot&) = delete;
    db_relation_slot(db_relation_slot&& other) noexcept
        : state_(std::exchange(other.state_, state_type::unset)),
          value_(std::move(other.value_)) {}
    db_relation_slot& operator=(db_relation_slot&&) = delete;
    state_type state_{state_type::unset};
    pointer value_;
    void reset() noexcept {
        value_.reset();
        state_ = state_type::unset;
    }
};

template <template <typename> class predicate_type, typename... ts_type>
struct tuple_filter;
template <template <typename> class predicate_type>
struct tuple_filter<predicate_type> {
    using type = std::tuple<>;
};
template <template <typename> class predicate_type, typename t_type, typename... ts_type>
struct tuple_filter<predicate_type, t_type, ts_type...> {
    using tail = typename tuple_filter<predicate_type, ts_type...>::type;
    using type = std::conditional_t<predicate_type<t_type>::value, decltype(std::tuple_cat(std::declval<std::tuple<t_type>>(), std::declval<tail>())), tail>;
};
template <template <typename> class predicate_type, typename... ts_type>
using tuple_filter_t = typename tuple_filter<predicate_type, ts_type...>::type;

}  // namespace detail

// Columns and projections require db_mapped_value: all text is owned.
// PostgreSQL arrays use std::vector of owning scalar or optional elements.
// Decoding retains one-dimensional element order and discards the lower bound;
// multidimensional arrays are rejected with db_conversion_error.
// Option combinations that cannot hold (see valid_db_column_options) do not
// name a column type, so the declaration fails to compile.
template <fixed_string field_name, db_mapped_value t_type, db_column_options options_type = db_column_options{}>
    requires(detail::valid_db_column_options<t_type>(options_type))
struct db_column final {
    static_assert(!detail::is_optional<t_type>::value, "use nullable column options and set_null for entity fields; optional is only for array elements");
    static constexpr auto name = field_name;
    using value_type = t_type;
    static consteval auto normalize_options(auto opts) noexcept {
        if (opts.delete_date_) {
            opts.nullable_ = true;
        }
        return opts;
    }
    static constexpr auto options = normalize_options(options_type);
    static constexpr db_data_type data_type = options_type.data_type_ == db_data_type::inferred
                                                  ? detail::db_entity_type_traits<t_type>::data_type
                                                  : options_type.data_type_;
};

template <typename t_type>
struct is_db_column : std::false_type {};
template <fixed_string name, typename t_type, db_column_options options_type>
struct is_db_column<db_column<name, t_type, options_type>> : std::true_type {};
template <typename t_type>
struct is_db_relation : std::false_type {};
template <fixed_string name, typename target_type, typename... joins_type>
struct is_db_relation<db_many_to_one<name, target_type, joins_type...>> : std::true_type {};
template <fixed_string name, typename target_type, typename... mapping_type>
struct is_db_relation<db_one_to_one<name, target_type, mapping_type...>> : std::true_type {};
template <fixed_string name, typename target_type, fixed_string inverse>
struct is_db_relation<db_one_to_many<name, target_type, inverse>> : std::true_type {};
template <fixed_string name, typename target_type, typename mapping_type>
struct is_db_relation<db_many_to_many<name, target_type, mapping_type>> : std::true_type {};

namespace detail {

template <fixed_string table, typename... members_type>
class db_entity_storage final {
    static_assert(((is_db_column<members_type>::value || is_db_relation<members_type>::value) && ...),
        "SQL entities require RUVIA_DB_COLUMN or SQL relation descriptors");
    using columns_tuple_type = tuple_filter_t<is_db_column, members_type...>;
    using relations_tuple_type = tuple_filter_t<is_db_relation, members_type...>;
    static_assert(unique_entity_columns<members_type...>(), "duplicate database entity member name");
    static consteval std::size_t count_lifecycle_columns(auto predicate) noexcept {
        return [&]<typename... cols>(std::tuple<cols...>*) {
            return (std::size_t{predicate(cols::options)} + ... + std::size_t{0});
        }(static_cast<columns_tuple_type*>(nullptr));
    }
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.create_date_; }) <= 1,
        "database entity may declare at most one create_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.update_date_; }) <= 1,
        "database entity may declare at most one update_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.delete_date_; }) <= 1,
        "database entity may declare at most one delete_date_ column");
    static_assert(count_lifecycle_columns([](const auto& opts) { return opts.version_; }) <= 1,
        "database entity may declare at most one version_ column");
    template <typename r_type>
    using relation_slot_type = db_relation_slot<typename r_type::target_entity_type, r_type::is_collection>;
    template <std::size_t... indexes>
    static auto make_relation_slots(std::index_sequence<indexes...>) {
        return std::tuple<relation_slot_type<std::tuple_element_t<indexes, relations_tuple_type>>...>{};
    }
    using column_storage = typename entity_storage_from_tuple<columns_tuple_type>::type;
    using relation_slots_type = decltype(make_relation_slots(std::make_index_sequence<std::tuple_size_v<relations_tuple_type>>{}));
    template <fixed_string name>
    static consteval std::size_t relation_index() {
        constexpr auto result_value = descriptor_index<relations_tuple_type, name>();
        static_assert(result_value < std::tuple_size_v<relations_tuple_type>, "unknown database entity relation");
        return result_value;
    }
    // Member names are unique across columns and relations.
    template <fixed_string name>
    static consteval bool is_relation() {
        return descriptor_index<relations_tuple_type, name>() < std::tuple_size_v<relations_tuple_type>;
    }
    template <fixed_string name>
    auto& relation_slot() {
        return std::get<relation_index<name>()>(relation_slots_);
    }
    template <fixed_string name>
    const auto& relation_slot() const {
        return std::get<relation_index<name>()>(relation_slots_);
    }

public:
    using columns_type = columns_tuple_type;
    using relations_type = relations_tuple_type;
    static constexpr std::string_view table_name() noexcept {
        return table.view();
    }
    template <fixed_string name>
    static consteval std::size_t column_index() {
        return column_storage::template column_index<name>();
    }
    template <fixed_string name>
    static consteval std::string_view column_name() noexcept {
        return column_storage::template column_name<name>();
    }
    db_entity_storage() = default;
    db_entity_storage(const db_entity_storage&) = delete;
    db_entity_storage& operator=(const db_entity_storage&) = delete;
    db_entity_storage(db_entity_storage&&) noexcept = default;
    template <fixed_string name>
    auto& get() & RUVIA_LIFETIMEBOUND {
        if constexpr (is_relation<name>()) {
            auto& s = relation_slot<name>();
            if (s.state_ != decltype(s.state_)::value) {
                throw std::logic_error("database entity relation is not loaded");
            }
            return *s.value_;
        } else {
            return columns_.template get<name>();
        }
    }
    template <fixed_string name>
    auto& get() && = delete;
    template <fixed_string name>
    const auto& get() const& RUVIA_LIFETIMEBOUND {
        return const_cast<db_entity_storage*>(this)->get<name>();
    }
    template <fixed_string name>
    const auto& get() const&& = delete;
    template <fixed_string name, typename v_type>
    void set(v_type&& value) {
        static_assert(!is_relation<name>(), "relations cannot be assigned; use relation loading access");
        columns_.template set<name>(std::forward<v_type>(value));
    }
    template <fixed_string name>
    void set_null()
        requires(!is_relation<name>() && std::tuple_element_t<column_index<name>(), columns_type>::options.nullable_)
    {
        columns_.template set_null<name>();
    }
    template <fixed_string name>
    void reset() {
        if constexpr (is_relation<name>()) {
            relation_slot<name>().reset();
        } else {
            columns_.template reset<name>();
        }
    }
    template <fixed_string name>
    bool is_set() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ != decltype(relation_slot<name>().state_)::unset;
        } else {
            return columns_.template is_set<name>();
        }
    }
    template <fixed_string name>
    bool is_null() const {
        if constexpr (is_relation<name>()) {
            return relation_slot<name>().state_ == decltype(relation_slot<name>().state_)::null;
        } else {
            return columns_.template is_null<name>();
        }
    }

private:
    template <typename>
    friend struct db_entity_access;
    column_storage columns_;
    relation_slots_type relation_slots_;
};

template <typename e_type>
struct db_entity_access final {
    static auto& storage(e_type& entity) noexcept {
        return entity.entity_;
    }
    template <fixed_string name>
    static auto& emplace_relation(e_type& entity) {
        auto& slot = relation_slot<name, false>(entity);
        using stored_type = typename std::remove_reference_t<decltype(slot)>::stored_type;
        slot.value_ = std::make_unique<stored_type>();
        slot.state_ = decltype(slot.state_)::value;
        return *slot.value_;
    }

    template <fixed_string name>
    static auto& ensure_relation_collection(e_type& entity) {
        auto& slot = relation_slot<name, true>(entity);
        if (slot.state_ != decltype(slot.state_)::value) {
            using stored_type = typename std::remove_reference_t<decltype(slot)>::stored_type;
            slot.value_ = std::make_unique<stored_type>();
            slot.state_ = decltype(slot.state_)::value;
        }
        return *slot.value_;
    }

    template <fixed_string name>
    static void set_relation_null(e_type& entity) {
        auto& slot = relation_slot<name, false>(entity);
        slot.reset();
        slot.state_ = decltype(slot.state_)::null;
    }

private:
    template <fixed_string name, bool expected_collection>
    static auto& relation_slot(e_type& entity) noexcept {
        auto& owner_value = storage(entity);
        using owner_type = std::remove_reference_t<decltype(owner_value)>;
        static_assert(owner_type::template is_relation<name>());
        using relation_type = std::tuple_element_t<owner_type::template relation_index<name>(), typename e_type::relations_type>;
        if constexpr (expected_collection) {
            static_assert(relation_type::is_collection, "ensure_relation_collection requires a collection relation");
        } else {
            static_assert(!relation_type::is_collection, "to-one relation operation is not valid on a collection relation");
        }
        return owner_value.template relation_slot<name>();
    }
};

template <typename columns>
inline constexpr bool sql_columns = []<std::size_t... indices>(std::index_sequence<indices...>) {
    return (is_db_column<std::tuple_element_t<indices, columns>>::value && ...);
}(std::make_index_sequence<std::tuple_size_v<columns>>{});

}  // namespace detail

template <typename entity>
concept sql_entity = requires(const entity& value) {
    typename entity::columns_type;
    typename entity::relations_type;
    typename entity::sql_entity_type;
    requires std::same_as<entity, typename entity::sql_entity_type>;
    requires detail::sql_columns<typename entity::columns_type>;
    { entity::table_name() } -> std::convertible_to<std::string_view>;
};

template <typename projection>
concept sql_projection = requires(const projection& value) {
    typename projection::columns_type;
    typename projection::db_projection_type;
    requires std::same_as<projection, typename projection::db_projection_type>;
    requires detail::sql_columns<typename projection::columns_type>;
};

template <typename e_type, fixed_string field_name>
class db_field_reference final {
    static_assert(sql_entity<e_type>, "SQL field references require a SQL entity");
    static_assert(e_type::template column_index<field_name>() < std::tuple_size_v<typename e_type::columns_type>);
    using column_type = std::tuple_element_t<e_type::template column_index<field_name>(), typename e_type::columns_type>;
    static constexpr bool is_writable = detail::is_writable_db_column<column_type>();
    static constexpr bool is_numeric = detail::is_numeric_db_column<column_type>();

public:
    constexpr db_field_reference(borrowed_text alias = {}) noexcept
        : alias_(alias) {}
    constexpr db_field_reference(const db_field_reference&) noexcept = default;
    constexpr db_field_reference(db_field_reference&&) noexcept = default;
    constexpr db_field_reference& operator=(const db_field_reference&) noexcept = default;
    constexpr db_field_reference& operator=(db_field_reference&&) noexcept = default;

    [[nodiscard]] static constexpr std::string_view name() noexcept {
        return field_name.view();
    }
    [[nodiscard]] static constexpr std::string_view table_name() noexcept {
        return e_type::table_name();
    }
    [[nodiscard]] constexpr std::string_view alias() const noexcept {
        return alias_;
    }
    [[nodiscard]] constexpr db_field_reference with_alias(borrowed_text alias) const noexcept {
        return db_field_reference(alias);
    }
    [[nodiscard]] db_expression expression(db_query& query, std::string_view fallback_alias = {}) const {
        const auto qualifier = !alias_.empty() ? alias_.view() : (fallback_alias.empty() ? e_type::table_name() : fallback_alias);
        return query.column(field_name.view(), qualifier);
    }
    [[nodiscard]] db_expression excluded(db_query& query) const {
        return query.excluded(field_name.view());
    }
    [[nodiscard]] db_expr to_expr() const {
        return db_expr(detail::db_owned_expression::make_static_column(
            field_name.view(), e_type::table_name(), alias_));
    }
    [[nodiscard]] operator db_expr() const {
        return to_expr();
    }
    [[nodiscard]] db_expr excluded_expr() const noexcept {
        return db_expr(detail::db_owned_expression::make_static_excluded(field_name.view()));
    }
    [[nodiscard]] db_order_term asc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().asc(nulls);
    }
    [[nodiscard]] db_order_term desc(db_nulls_order nulls = db_nulls_order::default_value) const {
        return to_expr().desc(nulls);
    }

    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_distinct_from(v_type&& value) const {
        return to_expr().is_distinct_from(std::forward<v_type>(value));
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_predicate is_not_distinct_from(v_type&& value) const {
        return to_expr().is_not_distinct_from(std::forward<v_type>(value));
    }
    [[nodiscard]] db_predicate is_distinct_from(const db_expr& rhs) const {
        return to_expr().is_distinct_from(rhs);
    }
    [[nodiscard]] db_predicate is_not_distinct_from(const db_expr& rhs) const {
        return to_expr().is_not_distinct_from(rhs);
    }

    [[nodiscard]] db_predicate like(std::string_view pattern) const {
        return to_expr().like(pattern);
    }
    [[nodiscard]] db_predicate not_like(std::string_view pattern) const {
        return to_expr().not_like(pattern);
    }
    [[nodiscard]] db_predicate ilike(std::string_view pattern) const {
        return to_expr().ilike(pattern);
    }
    [[nodiscard]] db_predicate not_ilike(std::string_view pattern) const {
        return to_expr().not_ilike(pattern);
    }
    [[nodiscard]] db_predicate regex(std::string_view pattern) const {
        return to_expr().regex(pattern);
    }
    [[nodiscard]] db_predicate iregex(std::string_view pattern) const {
        return to_expr().iregex(pattern);
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    template <detail::db_parameter lower_type, detail::db_parameter upper_type>
    [[nodiscard]] db_predicate not_between(lower_type&& lower, upper_type&& upper) const {
        return to_expr().not_between(std::forward<lower_type>(lower), std::forward<upper_type>(upper));
    }
    [[nodiscard]] db_predicate is_null() const {
        return to_expr().is_null();
    }
    [[nodiscard]] db_predicate is_not_null() const {
        return to_expr().is_not_null();
    }
    [[nodiscard]] db_predicate is_true() const {
        return to_expr().is_true();
    }
    [[nodiscard]] db_predicate is_false() const {
        return to_expr().is_false();
    }
    [[nodiscard]] db_predicate is_not_true() const {
        return to_expr().is_not_true();
    }
    [[nodiscard]] db_predicate is_not_false() const {
        return to_expr().is_not_false();
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate in(std::initializer_list<t_type> values) const {
        return in(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::span<const t_type> values) const {
        return compare_list(db_binary_operator::not_in, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate not_in(std::initializer_list<t_type> values) const {
        return not_in(std::span<const t_type>(values.begin(), values.size()));
    }
    [[nodiscard]] db_predicate in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery);
    }
    [[nodiscard]] db_predicate not_in(const db_query& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery);
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::in, subquery.query());
    }
    template <typename subquery_type>
        requires requires(const subquery_type& s) {
            { s.query() } -> std::same_as<const db_query&>;
        }
    [[nodiscard]] db_predicate not_in(const subquery_type& subquery) const {
        return compare_subquery(db_binary_operator::not_in, subquery.query());
    }

    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contains, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contains(std::initializer_list<t_type> values) const {
        return array_contains(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_contained_by, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_contained_by(std::initializer_list<t_type> values) const {
        return array_contained_by(std::span<const t_type>(values.begin(), values.size()));
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::span<const t_type> values) const {
        return compare_array(db_binary_operator::array_overlap, values);
    }
    template <typename t_type>
    [[nodiscard]] db_predicate array_overlap(std::initializer_list<t_type> values) const {
        return array_overlap(std::span<const t_type>(values.begin(), values.size()));
    }

    [[nodiscard]] db_predicate json_contains(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contains, json_text);
    }
    [[nodiscard]] db_predicate json_contained_by(std::string_view json_text) const {
        return compare_json(db_binary_operator::json_contained_by, json_text);
    }
    [[nodiscard]] db_predicate json_has_key(std::string_view key) const {
        return to_expr().compare_expr(db_binary_operator::json_has_key, db_expr::value(key));
    }
    [[nodiscard]] db_predicate json_has_any_key(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_any_key, keys);
    }
    [[nodiscard]] db_predicate json_has_any_key(std::initializer_list<std::string_view> keys) const {
        return json_has_any_key(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::span<const std::string_view> keys) const {
        return compare_json_keys(db_binary_operator::json_has_all_keys, keys);
    }
    [[nodiscard]] db_predicate json_has_all_keys(std::initializer_list<std::string_view> keys) const {
        return json_has_all_keys(std::span<const std::string_view>(keys.begin(), keys.size()));
    }
    [[nodiscard]] db_predicate json_text_equals(std::string_view key, std::string_view value) const {
        return to_expr().json_get_text(key) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::span<const std::string_view> path, std::string_view value) const {
        return to_expr().json_path_text(path) == value;
    }
    [[nodiscard]] db_predicate json_path_text_equals(std::initializer_list<std::string_view> path, std::string_view value) const {
        return json_path_text_equals(std::span<const std::string_view>(path.begin(), path.size()), value);
    }

    [[nodiscard]] db_selection select() const {
        return as(field_name.view());
    }
    [[nodiscard]] db_selection as(borrowed_text alias_name) const {
        return to_expr().as(alias_name);
    }
    template <detail::db_parameter v_type>
    [[nodiscard]] db_selection coalesce(v_type&& fallback, borrowed_text alias_name = field_name.view()) const {
        return db_expr::coalesce({to_expr(), db_expr::value(std::forward<v_type>(fallback))}).as(alias_name);
    }
    [[nodiscard]] db_selection coalesce_text(std::string_view fallback = {}, borrowed_text alias_name = field_name.view()) const {
        auto col_expr = to_expr();
        if constexpr (column_type::data_type != db_data_type::text &&
                      column_type::data_type != db_data_type::varchar &&
                      column_type::data_type != db_data_type::char_value) {
            col_expr = col_expr.cast(db_data_type::text);
        }
        return db_expr::coalesce({col_expr, db_expr::value(fallback)}).as(alias_name);
    }

    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment set(v_type&& value) const {
        return db_assignment{field_name.view(), db_expr::value(std::forward<v_type>(value))};
    }
    template <detail::db_parameter v_type>
        requires(is_writable && (!std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t> || column_type::options.nullable_))
    [[nodiscard]] db_assignment operator=(v_type&& value) const {
        return set(std::forward<v_type>(value));
    }
    [[nodiscard]] db_assignment set(const db_expr& value) const
        requires is_writable
    {
        if (value.empty()) {
            throw std::invalid_argument("cannot assign an empty database expression");
        }
        return db_assignment{field_name.view(), value};
    }
    [[nodiscard]] db_assignment operator=(const db_expr& value) const
        requires is_writable
    {
        return set(value);
    }
    [[nodiscard]] db_assignment set_null() const
        requires(is_writable && column_type::options.nullable_)
    {
        return db_assignment{field_name.view(), db_expr::null()};
    }
    [[nodiscard]] db_assignment set_null_if_empty(std::string_view value) const
        requires(is_writable && column_type::options.nullable_)
    {
        auto expr = db_expr::null_if(db_expr::value(value), db_expr::value(std::string_view{}));
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            expr = expr.cast({.custom_name_ = column_type::options.enum_name_.view()});
        } else if constexpr (column_type::data_type != db_data_type::inferred &&
                             column_type::data_type != db_data_type::text &&
                             column_type::data_type != db_data_type::varchar &&
                             column_type::data_type != db_data_type::char_value) {
            expr = expr.cast({.data_type_ = column_type::data_type,
                .length_ = column_type::options.length_,
                .precision_ = column_type::options.precision_,
                .scale_ = column_type::options.scale_});
        }
        return db_assignment{field_name.view(), std::move(expr)};
    }
    [[nodiscard]] db_assignment set_default() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::default_value()};
    }
    [[nodiscard]] db_assignment set_now() const
        requires is_writable
    {
        return db_assignment{field_name.view(), db_expr::now()};
    }
    [[nodiscard]] db_assignment set_excluded() const
        requires is_writable
    {
        return db_assignment{field_name.view(), excluded_expr()};
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment increment(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() + value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator+=(number_type value) const {
        return increment(value);
    }
    template <typename number_type = std::int64_t>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment decrement(number_type value = number_type{1}) const {
        return db_assignment{field_name.view(), to_expr() - value};
    }
    template <typename number_type>
        requires(is_writable && is_numeric && (std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] db_assignment operator-=(number_type value) const {
        return decrement(value);
    }

private:
    template <typename t_type>
    [[nodiscard]] db_predicate compare_list(db_binary_operator op, std::span<const t_type> values) const {
        return db_predicate(detail::db_owned_expression::make_in_list(to_expr().owned(), op, values));
    }
    [[nodiscard]] db_predicate compare_subquery(db_binary_operator op, const db_query& subquery) const {
        return db_predicate(detail::db_owned_expression::make_in_subquery(to_expr().owned(), op, subquery));
    }
    [[nodiscard]] db_predicate compare_json(db_binary_operator op, std::string_view json_text) const {
        return to_expr().compare_expr(op, db_expr::value(json_text).cast(db_data_type::jsonb));
    }
    [[nodiscard]] db_predicate compare_json_keys(db_binary_operator op, std::span<const std::string_view> keys) const {
        return db_predicate(detail::db_owned_expression::make_json_path_op(to_expr().owned(), op, keys));
    }
    template <typename t_type>
    db_predicate compare_array(db_binary_operator op, std::span<const t_type> values) const {
        static_assert(detail::is_std_vector<typename column_type::value_type>::value, "array predicates require an array column");
        using item_type = typename detail::is_std_vector<typename column_type::value_type>::value_type;
        constexpr auto type = column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array
                                  ? detail::db_entity_type_traits<item_type>::data_type
                                  : column_type::options.data_type_;
        const db_type_definition array_type{
            .data_type_ = type,
            .length_ = column_type::options.length_,
            .precision_ = column_type::options.precision_,
            .scale_ = column_type::options.scale_,
            .array_ = true,
        };
        return db_predicate(detail::db_owned_expression::make_array_op(to_expr().owned(), op, values, array_type));
    }

    borrowed_text alias_{};
};

#define RUVIA_DB_COLUMN(name, value_type, ...) ::ruvia::db_column<::ruvia::fixed_string{#name}, value_type __VA_OPT__(, ) __VA_ARGS__>
#define RUVIA_DB_JOIN_COLUMN(local, referenced) ::ruvia::db_join_column<::ruvia::fixed_string{#local}, ::ruvia::fixed_string{#referenced}>
#define RUVIA_DB_JOIN_COLUMNS(...) ::ruvia::db_join_columns<__VA_ARGS__>
#define RUVIA_DB_INVERSE(name) ::ruvia::db_inverse<::ruvia::fixed_string{#name}>
#define RUVIA_DB_JOIN_TABLE(table, owner_columns_type, inverse_columns_type) ::ruvia::db_join_table<::ruvia::fixed_string{table}, owner_columns_type, inverse_columns_type>
#define RUVIA_DB_MANY_TO_ONE(name, target, ...) ::ruvia::db_many_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_ONE(name, target, ...) ::ruvia::db_one_to_one<::ruvia::fixed_string{#name}, target, __VA_ARGS__>
#define RUVIA_DB_ONE_TO_MANY(name, target, inverse) ::ruvia::db_one_to_many<::ruvia::fixed_string{#name}, target, ::ruvia::fixed_string{#inverse}>
#define RUVIA_DB_MANY_TO_MANY(name, target, mapping_type) ::ruvia::db_many_to_many<::ruvia::fixed_string{#name}, target, mapping_type>

// set<field>() 检查整数范围与有限浮点溢出，数组及 optional 元素递归使用相同规则。
// 整数列不隐式接受浮点输入；调用方须显式选择转换策略。失败保留原值和 unset/null 状态。
// 此保证不拦截调用方通过可变 get() 或显式转换自行执行的数值赋值。
#define RUVIA_DB_ENTITY(name, table, ...)                                                                                   \
    struct name final {                                                                                                     \
    private:                                                                                                                \
        using storage_type = ::ruvia::detail::db_entity_storage<::ruvia::fixed_string{table}, __VA_ARGS__>;                 \
        storage_type entity_;                                                                                               \
        template <typename>                                                                                                 \
        friend struct ::ruvia::detail::db_entity_access;                                                                    \
        template <typename, bool>                                                                                           \
        friend struct ::ruvia::detail::db_relation_slot;                                                                    \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, entity_)                                                          \
        using sql_entity_type = name;                                                                                       \
        using columns_type = typename storage_type::columns_type;                                                           \
        using relations_type = typename storage_type::relations_type;                                                       \
        static constexpr std::string_view table_name() noexcept {                                                           \
            return storage_type::table_name();                                                                              \
        }                                                                                                                   \
        template <::ruvia::fixed_string field_name>                                                                         \
        static constexpr ::ruvia::db_field_reference<name, field_name> column(::ruvia::borrowed_text alias = {}) noexcept { \
            return ::ruvia::db_field_reference<name, field_name>(alias);                                                    \
        }                                                                                                                   \
        RUVIA_DETAIL_ENTITY_VALUE_API(entity_)                                                                              \
    };
namespace detail {
template <typename... columns>
struct db_projection_storage final {
    static_assert((::ruvia::is_db_column<columns>::value && ...));
    using type = ::ruvia::detail::entity_value_storage<columns...>;
};
}  // namespace detail

#define RUVIA_DB_PROJECTION(name, ...)                                                           \
    struct name final {                                                                          \
    private:                                                                                     \
        using storage_type = typename ::ruvia::detail::db_projection_storage<__VA_ARGS__>::type; \
        storage_type values_;                                                                    \
        RUVIA_DETAIL_ENTITY_OWNER_API(name, storage_type, values_)                               \
        using db_projection_type = name;                                                         \
        using columns_type = typename storage_type::columns_type;                                \
        RUVIA_DETAIL_ENTITY_VALUE_API(values_)                                                   \
    };

}  // namespace ruvia

namespace ruvia::detail {

[[noreturn]] inline void throw_invalid_postgresql_array() {
    throw db_conversion_error(db_conversion_error::code_type::invalid_format, "malformed one-dimensional PostgreSQL array");
}

struct postgresql_array_text final {
    std::string_view contents_;
    std::optional<std::size_t> elements_;
};

// PostgreSQL emits a dimension prefix only when the lower bound differs from
// one. A vector keeps the element sequence rather than the database subscripts.
[[nodiscard]] inline postgresql_array_text parse_postgresql_array_text(std::string_view source) {
    std::optional<std::size_t> elements;
    if (source.starts_with('[')) {
        std::int32_t lower{};
        std::int32_t upper{};
        const auto* const last = source.data() + source.size();
        const auto [lower_end, lower_error] = std::from_chars(source.data() + 1, last, lower);
        if (lower_error != std::errc{} || lower_end == last || *lower_end != ':') {
            throw_invalid_postgresql_array();
        }
        const auto [upper_end, upper_error] = std::from_chars(lower_end + 1, last, upper);
        if (upper_error != std::errc{} || upper_end == last || *upper_end != ']') {
            throw_invalid_postgresql_array();
        }
        // The server requires upper + 1 and the dimension length to fit int32.
        const auto length = std::int64_t{upper} - lower + 1;
        if (length <= 0 || length > (std::numeric_limits<std::int32_t>::max)() ||
            upper == (std::numeric_limits<std::int32_t>::max)() ||
            upper_end + 1 == last || upper_end[1] != '=') {
            throw_invalid_postgresql_array();
        }
        elements = static_cast<std::size_t>(length);
        source.remove_prefix(static_cast<std::size_t>(upper_end + 2 - source.data()));
    }
    if (source.size() < 2 || source.front() != '{' || source.back() != '}') {
        throw_invalid_postgresql_array();
    }
    return {source, elements};
}

template <typename visitor_type, typename reserve_type = void (*)(std::size_t)>
void for_each_postgresql_array_element(std::string_view source, visitor_type&& visit, reserve_type&& reserve = +[](std::size_t) noexcept {}) {
    const auto array = parse_postgresql_array_text(source);
    const auto contents = array.contents_;
    if (array.elements_) {
        // Every element occupies at least one byte plus a separator inside the
        // braces, so the arrived text bounds the declared length before reserve.
        if (*array.elements_ > (contents.size() - 1) / 2) {
            throw_invalid_postgresql_array();
        }
        reserve(*array.elements_);
    }
    std::string escaped_token;
    std::size_t element_count = 0;
    std::size_t token_start = 1;
    std::size_t chunk_start = 1;
    std::size_t token_end = 1;
    bool quoted = false;
    bool quoted_token = false;
    bool quote_closed = false;
    bool escaped = false;
    bool has_escapes = false;
    auto append = [&](std::size_t default_end) {
        const std::size_t end = quote_closed ? token_end : default_end;
        if (quoted || escaped || (end == token_start && !quoted_token) ||
            (array.elements_ && element_count >= *array.elements_)) {
            throw_invalid_postgresql_array();
        }
        std::string_view token;
        if (has_escapes) {
            if (end > chunk_start) {
                escaped_token.append(contents.data() + chunk_start, end - chunk_start);
            }
            token = escaped_token;
        } else {
            token = contents.substr(token_start, end - token_start);
        }
        ++element_count;
        if (token == "NULL" && !quoted_token) {
            visit(std::optional<std::string_view>{});
        } else {
            visit(std::optional<std::string_view>{token});
        }
        token_start = default_end + 1;
        chunk_start = token_start;
        token_end = token_start;
        quoted = false;
        quoted_token = false;
        quote_closed = false;
        has_escapes = false;
    };
    for (std::size_t i = 1; i + 1 < contents.size(); ++i) {
        const char ch = contents[i];
        if (escaped) {
            chunk_start = i;
            escaped = false;
        } else if (ch == '\\') {
            if (quote_closed) {
                throw_invalid_postgresql_array();
            }
            if (!has_escapes) {
                has_escapes = true;
                escaped_token.clear();
            }
            if (i > chunk_start) {
                escaped_token.append(contents.data() + chunk_start, i - chunk_start);
            }
            escaped = true;
            quoted_token = true;
        } else if (ch == '"') {
            if (quoted) {
                quoted = false;
                quote_closed = true;
                token_end = i;
            } else {
                if (i != token_start || quoted_token || quote_closed) {
                    throw_invalid_postgresql_array();
                }
                quoted = true;
                quoted_token = true;
                token_start = i + 1;
                chunk_start = i + 1;
            }
        } else if (ch == ',' && !quoted) {
            append(i);
        } else {
            if (quote_closed || (!quoted && (ch == '{' || ch == '}'))) {
                throw_invalid_postgresql_array();
            }
        }
    }
    if (contents.size() > 2) {
        append(contents.size() - 1);
    }
    if (array.elements_ && element_count != *array.elements_) {
        throw_invalid_postgresql_array();
    }
}

template <db_mapped_value t_type>
[[nodiscard]] t_type decode_array_element(std::string_view text) {
    if constexpr (is_optional<t_type>::value) {
        using v_type = typename is_optional<t_type>::value_type;
        return decode_array_element<v_type>(text);
    } else {
        return convert_db_text<t_type>(text);
    }
}

template <db_mapped_value t_type>
void decode_db_field(const db_field& field, t_type& out) {
    using u_type = std::remove_cv_t<t_type>;
    if constexpr (is_optional<u_type>::value) {
        using v_type = typename is_optional<u_type>::value_type;
        if (!field.value()) {
            out.reset();
            return;
        }
        auto& inner = out.emplace(entity_value_slot<v_type>::make_value());
        decode_db_field(field, inner);
    } else if constexpr (is_std_vector<u_type>::value) {
        using v_type = typename is_std_vector<u_type>::value_type;
        const auto source_value = field.value();
        if (!source_value) {
            throw_invalid_postgresql_array();
        }
        out.clear();
        for_each_postgresql_array_element(
            *source_value,
            [&out](std::optional<std::string_view> element) {
                if (!element) {
                    if constexpr (is_optional<v_type>::value) {
                        out.emplace_back(std::nullopt);
                    } else {
                        throw db_conversion_error(
                            db_conversion_error::code_type::invalid_format, "NULL array element");
                    }
                } else {
                    out.push_back(decode_array_element<v_type>(*element));
                }
            },
            [&out](std::size_t count) { out.reserve(count); });
    } else if constexpr (std::is_same_v<u_type, std::string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign(*value);
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    } else if constexpr (std::is_same_v<u_type, string>) {
        if (auto value = field.as<std::string_view>()) {
            out.assign_owned(*value);
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    } else {
        if (auto value = field.as<u_type>()) {
            out = *value;
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable entity column");
        }
    }
}

template <typename e_type, typename c_type>
void decode_entity_field(e_type& entity, const db_field& field) {
    const auto raw = field.value();
    if (!raw) {
        if constexpr (c_type::options.nullable_) {
            entity.template set_null<c_type::name>();
            return;
        } else {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "NULL in non-nullable database entity column");
        }
    }
    using value_type = typename c_type::value_type;
    if constexpr (std::is_same_v<value_type, std::string> || std::is_same_v<value_type, string>) {
        entity.template set<c_type::name>(*raw);
    } else {
        auto value = entity_value_slot<value_type>::make_value();
        decode_db_field(field, value);
        entity.template set<c_type::name>(std::move(value));
    }
}

// SQL permits repeated result names; a mapped column must name exactly one.
struct db_result_column_lookup final {
    unique_lookup_status state_{unique_lookup_status::absent};
    std::size_t index_{0};
};

[[nodiscard]] inline db_result_column_lookup find_unique_db_result_column(
    std::span<const std::string> names, std::string_view name) noexcept {
    db_result_column_lookup result;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (names[index] == name) {
            if (result.state_ == unique_lookup_status::found) {
                return {unique_lookup_status::repeated, index};
            }
            result = {unique_lookup_status::found, index};
        }
    }
    return result;
}

template <typename e_type>
class db_entity_row_decoder final {
    using columns_type = typename e_type::columns_type;
    static constexpr auto column_count = std::tuple_size_v<columns_type>;
    static constexpr auto missing_column = (std::numeric_limits<std::size_t>::max)();

public:
    explicit db_entity_row_decoder(const std::array<bool, column_count>& selected, bool wildcard = false)
        : selected_(selected),
          wildcard_(wildcard) {}

    e_type decode(const db_row& row) {
        const auto names = row.column_names();
        if (!schema_ || schema_->data() != names.data() || schema_->size() != names.size()) {
            // Entity column names are unique, so a result naming exactly the
            // entity columns in declaration order cannot repeat a name.
            bool ordered = names.size() == column_count;
            for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
                ordered = ordered && names[i] == column_type::name.view();
            });
            for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
                if (ordered) {
                    indices_[i] = i;
                    return;
                }
                if (!selected_[i] && !wildcard_) {
                    indices_[i] = missing_column;
                    return;
                }
                const auto lookup = find_unique_db_result_column(names, column_type::name.view());
                if (lookup.state_ == unique_lookup_status::repeated) {
                    throw db_conversion_error(
                        db_conversion_error::code_type::invalid_format, "database result has ambiguous mapped column names");
                }
                indices_[i] = lookup.state_ == unique_lookup_status::found ? lookup.index_ : missing_column;
            });
            schema_ = names;
        }
        e_type result;
        for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
            if (!selected_[i] && (!wildcard_ || indices_[i] == missing_column)) {
                return;
            }
            if (indices_[i] == missing_column) {
                throw db_conversion_error(
                    db_conversion_error::code_type::missing_column, "database result has no such column");
            }
            decode_entity_field<e_type, column_type>(result, row[indices_[i]]);
        });
        return result;
    }

private:
    std::array<bool, column_count> selected_{};
    std::array<std::size_t, column_count> indices_;
    bool wildcard_{false};
    // Used only while synchronously mapping a live, immutable db_rows result.
    std::optional<std::span<const std::string>> schema_{};
};

template <typename t_type>
void append_entity_array_value(std::string& output, const t_type& value) {
    if constexpr (is_optional<t_type>::value) {
        if (value) {
            append_entity_array_value(output, *value);
        } else {
            output += "NULL";
        }
    } else if constexpr (std::is_same_v<t_type, std::string> || std::is_same_v<t_type, string>) {
        output.push_back('"');
        auto remaining = std::string_view(value);
        while (!remaining.empty()) {
            const auto next_value = std::ranges::find_if(remaining, [](char ch) noexcept {
                return ch == '"' || ch == '\\' || ch == '\0';
            });
            if (next_value == remaining.end()) {
                output.append(remaining);
                break;
            }
            const auto special = static_cast<std::size_t>(next_value - remaining.begin());
            if (special != 0) {
                output.append(remaining.data(), special);
            }
            auto escaped_end = special;
            do {
                const char ch = remaining[escaped_end];
                if (ch == '\0') {
                    throw std::invalid_argument("PostgreSQL array text cannot contain NUL");
                }
                output.push_back('\\');
                output.push_back(ch);
                ++escaped_end;
            } while (escaped_end < remaining.size() &&
                     (remaining[escaped_end] == '"' || remaining[escaped_end] == '\\' || remaining[escaped_end] == '\0'));
            remaining.remove_prefix(escaped_end);
        }
        output.push_back('"');
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output += value ? "t" : "f";
    } else if constexpr (std::is_floating_point_v<t_type>) {
        append_db_number(output, value);
    } else if constexpr (std::is_signed_v<t_type>) {
        append_db_number(output, static_cast<std::int64_t>(value));
    } else {
        append_db_number(output, static_cast<std::uint64_t>(value));
    }
}

template <typename t_type>
db_value entity_db_value(const t_type& value) {
    if constexpr (std::is_same_v<t_type, std::string> || std::is_same_v<t_type, string>) {
        return db_value(std::string_view(value));
    } else if constexpr (std::is_same_v<t_type, bool> || std::is_arithmetic_v<t_type>) {
        return db_value(value);
    } else if constexpr (is_std_vector<t_type>::value) {
        std::string encoded;
        encoded.reserve(2U + value.size() * 4U);
        encoded.push_back('{');
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                encoded.push_back(',');
            }
            first = false;
            if constexpr (std::is_same_v<typename is_std_vector<t_type>::value_type, bool>) {
                append_entity_array_value(encoded, static_cast<bool>(item));
            } else {
                append_entity_array_value(encoded, item);
            }
        }
        encoded.push_back('}');
        return db_value_access::owned_string(std::move(encoded));
    } else {
        static_assert(std::is_same_v<t_type, void>, "unsupported entity value type");
    }
}

}  // namespace ruvia::detail

namespace ruvia {

// Decoding owns every text/array element independently of db_rows. SQL NULL
// requires an optional destination; malformed values keep db_conversion_error.
template <db_mapped_value value_type>
[[nodiscard]] value_type decode_db_field(const db_field& field) {
    auto result = detail::entity_value_slot<value_type>::make_value();
    detail::decode_db_field(field, result);
    return result;
}

// A projection describes a subset of columns. Every declared column is required
// and must be unique in the result; unrelated result columns are ignored.
template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_type decode_db_row(const db_row& row) {
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    return decoder.decode(row);
}

template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_rows<entity_type> decode_db_rows(const db_rows& rows) {
    auto result = detail::db_result_access::make_entity_rows<entity_type>(rows.size());
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    for (const auto& row : rows) {
        result.push_back(decoder.decode(row));
    }
    return result;
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename entity_type, fixed_string name>
struct db_relation_named final {
    static constexpr auto index = descriptor_index<typename entity_type::relations_type, name>();
    static_assert(index < std::tuple_size_v<typename entity_type::relations_type>, "unknown inverse database relation");
    using type = std::tuple_element_t<index, typename entity_type::relations_type>;
};

template <typename tuple_type>
struct reverse_db_join_columns;
template <typename... columns_type>
struct reverse_db_join_columns<std::tuple<columns_type...>> final {
    using type = std::tuple<db_join_column<columns_type::referenced, columns_type::local>...>;
};

// An owning one-to-one relation stores its join columns exactly like an owning
// many-to-one relation, so both normalize to the same mapping.
template <typename relation_type>
consteval db_relation_kind db_relation_mapping_kind() noexcept {
    return relation_type::kind == db_relation_kind::one_to_one && relation_type::is_owning
               ? db_relation_kind::many_to_one
               : relation_type::kind;
}

// One normalized mapping is consumed by schema generation and query planning.
// Target metadata is deliberately inspected only when a mapping is consumed,
// so mutually recursive and self-referential entity declarations stay valid.
template <typename source, typename relation_type,
    db_relation_kind kind = db_relation_mapping_kind<relation_type>(), bool owning = relation_type::is_owning>
struct db_relation_mapping;

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_one, true> {
    static constexpr bool through_join_table = false;
    using join_columns_type = typename relation_type::join_columns_type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_one && inverse_type::is_owning,
        "one-to-many must refer to an owning many-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::one_to_one, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::one_to_one && inverse_type::is_owning,
        "inverse one-to-one must refer to an owning one-to-one relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = false;
    using join_columns_type = typename reverse_db_join_columns<typename inverse_type::join_columns_type>::type;
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, true> {
    static constexpr bool through_join_table = true;
    using table = typename relation_type::join_table_type;
    using source_columns_type = typename table::owner_columns_type;
    using target_columns_type = typename table::inverse_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename source, typename relation_type>
struct db_relation_mapping<source, relation_type, db_relation_kind::many_to_many, false> {
    using inverse_type = typename db_relation_named<typename relation_type::target_entity_type, relation_type::inverse_name>::type;
    static_assert(inverse_type::kind == db_relation_kind::many_to_many && inverse_type::is_owning,
        "inverse many-to-many must refer to an owning many-to-many relation");
    static_assert(std::is_same_v<typename inverse_type::target_entity_type, source>,
        "inverse database relation must target its source entity");
    static constexpr bool through_join_table = true;
    using table = typename inverse_type::join_table_type;
    using source_columns_type = typename table::inverse_columns_type;
    using target_columns_type = typename table::owner_columns_type;
    static constexpr std::string_view table_name() noexcept {
        return table::name.view();
    }
};

template <typename columns_type>
consteval bool unique_db_join_columns() {
    constexpr auto count = std::tuple_size_v<columns_type>;
    std::array<std::string_view, count> local{}, referenced{};
    for_each_descriptor<columns_type>([&]<typename column_type, std::size_t i> {
        local[i] = column_type::local.view();
        referenced[i] = column_type::referenced.view();
    });
    for (std::size_t i = 0; i < count; ++i) {
        if (local[i].empty() || referenced[i].empty()) {
            return false;
        }
        for (std::size_t j = i + 1; j < count; ++j) {
            if (local[i] == local[j] || referenced[i] == referenced[j]) {
                return false;
            }
        }
    }
    return true;
}

template <typename source, typename relation_type>
constexpr void validate_db_relation() {
    using target_type = typename relation_type::target_entity_type;
    using mapping_type = db_relation_mapping<source, relation_type>;
    if constexpr (mapping_type::through_join_table) {
        using owner = typename mapping_type::source_columns_type;
        using other_type = typename mapping_type::target_columns_type;
        static_assert(std::tuple_size_v<owner> > 0 && std::tuple_size_v<other_type> > 0,
            "a join table requires both owner and inverse columns");
        static_assert(unique_db_join_columns<owner>() && unique_db_join_columns<other_type>(),
            "join table column mappings must be nonempty and unique");
        static_assert([] {
            bool unique = true;
            for_each_descriptor<owner>([&]<typename left_type, std::size_t> {
                for_each_descriptor<other_type>([&]<typename right_type, std::size_t> {
                    unique &= left_type::local.view() != right_type::local.view();
                });
            });
            return unique;
        }(),
            "join table owner and inverse columns must have distinct names");
        for_each_descriptor<owner>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::referenced>();
        });
        for_each_descriptor<other_type>([]<typename column_type, std::size_t> {
            (void)target_type::template column_index<column_type::referenced>();
        });
    } else {
        using columns_type = typename mapping_type::join_columns_type;
        static_assert(std::tuple_size_v<columns_type> > 0, "a database relation requires join columns");
        static_assert(unique_db_join_columns<columns_type>(), "database relation column mappings must be nonempty and unique");
        for_each_descriptor<columns_type>([]<typename column_type, std::size_t> {
            (void)source::template column_index<column_type::local>();
            (void)target_type::template column_index<column_type::referenced>();
        });
    }
}

template <typename entity_type>
consteval std::size_t db_primary_key_count() {
    std::size_t count = 0;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        count += column_type::options.primary_key_ ? 1 : 0;
    });
    return count;
}

template <typename entity_type>
consteval auto db_primary_key_columns() {
    std::array<std::string_view, db_primary_key_count<entity_type>()> result_value{};
    std::size_t i = 0;
    for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t> {
        if constexpr (column_type::options.primary_key_) {
            result_value[i++] = column_type::name.view();
        }
    });
    return result_value;
}

}  // namespace ruvia::detail

namespace ruvia {

// Whether a migration and the row recording it commit together.
//
// PostgreSQL runs DDL inside transactions, so wrapping both is what keeps an
// interrupted migration from being applied but unrecorded -- and re-applied on
// the next start. A few statements are refused inside a transaction block
// (CREATE INDEX CONCURRENTLY, VACUUM, ALTER TYPE ... ADD VALUE before 12), and
// those name the exception rather than forcing every migration to give up
// atomicity for one of them.
//
// MariaDB commits DDL implicitly whatever this says, so it applies to
// PostgreSQL only.
enum class db_migration_atomicity : std::uint8_t {
    transactional,
    unwrapped,
};

struct db_migration_options final {
    std::string id_{};
    std::string sql_{};
    db_migration_atomicity atomicity_{db_migration_atomicity::transactional};
};

// Immutable owning migration descriptor. Migrations are startup data, so the
// descriptor accepts temporary strings without imposing a hidden lifetime
// requirement on the asynchronous runner.
//
// `sql` is exactly one statement. Neither backend accepts more than one per
// call -- libpq's extended protocol refuses multiple commands and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- so a schema change that
// needs several statements is several migrations. A trailing ';' is allowed.
// Comments follow the selected backend's syntax; PostgreSQL line comments end
// at either CR or LF.
//
// `id` identifies an applied migration for the rest of the schema's life. It is
// compared with the migrations table's collation, so ids that differ only in
// letter case are rejected up front rather than resolving differently per
// backend.
class db_migration final {
public:
    explicit db_migration(db_migration_options options)
        : id_(std::move(options.id_)),
          sql_(std::move(options.sql_)),
          atomicity_(options.atomicity_) {}

    db_migration(const db_migration& other)
        : id_(other.id_),
          sql_(other.sql_),
          atomicity_(other.atomicity_) {}
    db_migration(db_migration&&) = default;
    db_migration& operator=(const db_migration&) = default;
    db_migration& operator=(db_migration&&) = default;

    [[nodiscard]] std::string_view id() const& noexcept RUVIA_LIFETIMEBOUND {
        return id_;
    }
    std::string_view id() const&& = delete;

    [[nodiscard]] std::string_view sql() const& noexcept RUVIA_LIFETIMEBOUND {
        return sql_;
    }
    std::string_view sql() const&& = delete;

    [[nodiscard]] constexpr db_migration_atomicity atomicity() const noexcept {
        return atomicity_;
    }

private:
    std::string id_;
    std::string sql_;
    db_migration_atomicity atomicity_;
};

struct db_migrator_options final {
    borrowed_text table_{"ruvia_schema_migrations"};
    // Positive lock wait; integer counts mean milliseconds. MariaDB rounds up
    // to whole seconds; PostgreSQL rejects values beyond its int32 millisecond range.
    timeout_duration lock_timeout_{std::chrono::seconds(30)};
};

class db_migration_report final {
public:
    db_migration_report(const db_migration_report&) = delete;
    db_migration_report& operator=(const db_migration_report&) = delete;
    db_migration_report(db_migration_report&&) noexcept = default;
    db_migration_report& operator=(db_migration_report&& other);

    [[nodiscard]] std::span<const std::string> applied() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> applied() const&& = delete;
    [[nodiscard]] std::span<const std::string> skipped() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const std::string> skipped() const&& = delete;
    [[nodiscard]] bool changed() const noexcept;

private:
    friend class db_migrator;
    friend class detail::db_migration_runner;

    db_migration_report() noexcept;

    std::vector<std::string> applied_;
    std::vector<std::string> skipped_;
};

// Applies pending migrations on the caller's event loop, holding a backend lock
// so that concurrent deployers serialize. Like db_client it borrows the loop's
// io_context and blocking pool (for a DNS host); it opens one dedicated
// connection per run, independent of any pool and of
// db_config::pool_size_per_worker_, that carries the lock and every statement.
//
// On PostgreSQL a migration and the row recording it commit together unless the
// migration opts out. MariaDB commits DDL implicitly, so there they are two
// statements and an interruption between them leaves the change applied and
// unrecorded, to be retried on the next run: write MariaDB migrations to be
// re-applicable -- CREATE TABLE IF NOT EXISTS and friends -- or guard them.
//
// Every applied migration must have a recorded SHA-256 digest matching its SQL.
// Missing columns, NULL/empty digests and mismatches are rejected, never adopted
// from the current source. Upgrade legacy history only after independently
// verifying the SQL actually applied; then record that SQL's digest explicitly.
//
// db_config's timeouts apply to each step: without connect_timeout or
// query_timeout_ a stalled backend blocks the run indefinitely unless the
// run's own operation_options bound it.
class db_migrator final {
public:
    // The configuration and migration-table name are copied into options.resource;
    // their source PMR storage may be released after construction. The supplied
    // resource itself must outlive this migrator and its runs. An invalid loop,
    // or a DNS host on a loop without a blocking pool, throws
    // std::invalid_argument.
    db_migrator(event_loop loop, const db_config& config, const db_migrator_options& options = {});
    ~db_migrator();

    db_migrator(const db_migrator&) = delete;
    db_migrator& operator=(const db_migrator&) = delete;
    db_migrator(db_migrator&&) noexcept;
    db_migrator& operator=(db_migrator&&) noexcept;

    // Validates and copies the migration list before returning the cold task,
    // which owns every input and must run on the bound loop (std::logic_error
    // otherwise). `operation` bounds the whole run, connecting included; a
    // non-positive timeout throws std::invalid_argument at the call and an
    // already-stopped token throws db_error(cancelled) before connecting. Its
    // expiry or stop request closes the dedicated connection, which releases
    // the lock, and throws db_error(timeout / cancelled); a migration already
    // committed stays recorded.
    [[nodiscard]] task<db_migration_report> migrate(
        std::span<const db_migration> migrations, operation_options operation = {}) const&;
    task<db_migration_report> migrate(std::span<const db_migration>, operation_options = {}) const&& = delete;

private:
    class storage_type;
    struct storage_deleter_type final {
        void operator()(storage_type* storage) const noexcept;
    };
    using storage_owner_type = std::unique_ptr<storage_type, storage_deleter_type>;

    [[nodiscard]] static storage_owner_type make_storage(
        event_loop loop, const db_config& config, const db_migrator_options& options);

    storage_owner_type storage_;
};

class db_schema;

enum class db_table_lock_mode : std::uint8_t {
    access_share,
    row_share,
    row_exclusive,
    share_update_exclusive,
    share,
    share_row_exclusive,
    exclusive,
    access_exclusive,
};

struct db_variable_definition final {
    borrowed_text name_{};
    db_type_definition type_{};
    db_expression default_value_{};
};

// Structured PL/pgSQL for migration blocks and trigger functions. All inputs
// are consumed synchronously. The procedure owns its generated body in PMR.
// Failed begin_block() and apply() leave the prior body and control stack
// unchanged, so allocation failures may be caught and the operation retried.
class db_procedure final {
public:
    db_procedure() noexcept;
    db_procedure(const db_procedure&) = delete;
    db_procedure& operator=(const db_procedure&) = delete;
    db_procedure(db_procedure&&) noexcept = default;
    db_procedure& operator=(db_procedure&&) = delete;

    void declare_variable(const db_variable_definition& variable);
    void declare_row(std::string_view name, std::string_view table);
    void declare_record(std::string_view name);
    void assign(std::string_view target, db_expression value);
    void select_into(const db_query& query, std::span<const std::string_view> targets, bool strict = false);
    void execute(const db_query& query);
    void perform(db_expression expression);
    void apply(const db_schema& schema);
    void begin_if(db_expression condition);
    void else_if(db_expression condition);
    void otherwise();
    void end_if();
    void begin_block();
    void catch_sql_state(std::string_view state);
    void end_block();
    void begin_for_query(std::string_view target, const db_query& query);
    void begin_foreach(std::string_view target, db_expression array, unsigned slice = 0);
    void end_loop();
    void execute_dynamic(db_expression command, std::span<const db_expression> using_args = {});
    void execute_dynamic(db_expression command, std::initializer_list<db_expression> using_args) {
        execute_dynamic(command, std::span<const db_expression>(using_args.begin(), using_args.size()));
    }
    void lock_table(std::string_view table, db_table_lock_mode mode = db_table_lock_mode::access_exclusive, bool nowait = false);
    void return_value(db_expression expression = {});
    void raise_exception(std::string_view message, std::string_view sql_state = "P0001");
    [[nodiscard]] std::string body() const;

private:
    enum class block_type : std::uint8_t { if_value,
        else_value,
        begin,
        exception,
        loop };
    // Declarations publish the name only after their text is appended, so a
    // failed declaration leaves no variable behind.
    void require_unique_variable(std::string_view name) const;
    void append_expression(std::string& output, db_expression value) const;
    std::string declarations_;
    std::string statements_;
    std::vector<std::string> variables_;
    std::vector<block_type> blocks_;
};

enum class db_identity : std::uint8_t { none,
    by_default,
    always };
enum class db_referential_action : std::uint8_t { no_action,
    restrict,
    cascade,
    set_null,
    set_default };
enum class db_constraint_kind : std::uint8_t { primary_key,
    unique,
    foreign_key,
    check };
enum class db_drop_behavior : std::uint8_t { restrict,
    cascade };
enum class db_index_method : std::uint8_t { btree,
    hash,
    gin,
    gist,
    sp_gist,
    brin };
enum class db_trigger_timing : std::uint8_t { before,
    after,
    instead_of };
enum class db_trigger_event : std::uint8_t { insert,
    update,
    delete_value,
    truncate };
enum class db_function_language : std::uint8_t { plpgsql,
    sql };
enum class db_function_volatility : std::uint8_t { volatile_value,
    stable,
    immutable };

struct db_function_parameter final {
    borrowed_text name_{};
    db_type_definition type_{};
    db_expression default_value_{};
};

struct db_function_definition final {
    borrowed_text name_{};
    std::vector<db_function_parameter> parameters_{};
    db_type_definition return_type_{};
    bool returns_set_{false};
    db_function_language language_{db_function_language::plpgsql};
    db_function_volatility volatility_{db_function_volatility::volatile_value};
    bool strict_{false};
    bool replace_{false};
    borrowed_text body_{};
};

struct db_trigger_definition final {
    borrowed_text name_{};
    borrowed_text table_{};
    borrowed_text function_{};
    db_trigger_timing timing_{db_trigger_timing::before};
    std::vector<db_trigger_event> events_{};
    bool for_each_row_{true};
    std::vector<borrowed_text> update_columns_{};
    borrowed_text old_table_{};
    borrowed_text new_table_{};
    db_expression when_{};
    std::vector<borrowed_text> arguments_{};
};

struct db_schema_column final {
    borrowed_text name_{};
    db_type_definition type_{};
    bool nullable_{false};
    bool primary_key_{false};
    bool unique_{false};
    db_identity identity_{db_identity::none};
    db_expression default_value_{};
    db_generated_type generated_type_{db_generated_type::none};
    db_expression as_expression_{};
};

struct db_schema_constraint final {
    borrowed_text name_{};
    db_constraint_kind kind_{db_constraint_kind::check};
    std::vector<borrowed_text> columns_{};
    borrowed_text referenced_table_{};
    std::vector<borrowed_text> referenced_columns_{};
    db_referential_action on_delete_{db_referential_action::no_action};
    db_referential_action on_update_{db_referential_action::no_action};
    bool deferrable_{false};
    bool initially_deferred_{false};
    bool not_valid_{false};
    db_expression check_{};
};

struct db_table_definition final {
    borrowed_text name_{};
    std::vector<db_schema_column> columns_{};
    std::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};

struct db_index_key final {
    borrowed_text column_{};
    db_expression expression_{};
    db_order_direction order_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
    borrowed_text operator_class_{};
};

struct db_index_definition final {
    borrowed_text name_{};
    borrowed_text table_{};
    std::vector<db_index_key> keys_{};
    bool unique_{false};
    bool if_not_exists_{false};
    bool concurrently_{false};
    db_index_method method_{db_index_method::btree};
    std::vector<borrowed_text> include_{};
    db_expression where_{};
};

struct db_schema_options final {
    db_driver driver_{db_driver::unspecified};
};

struct db_column_default final {
    borrowed_text column_{};
    db_expression value_{};
};
struct db_column_generated final {
    borrowed_text column_{};
    db_expression expression_{};
};
struct db_entity_table_options final {
    std::vector<db_column_default> defaults_{};
    std::vector<db_column_generated> generated_columns_{};
    std::vector<db_schema_constraint> constraints_{};
    bool if_not_exists_{false};
    bool temporary_{false};
    bool unlogged_{false};
};
struct db_table_option final {
    borrowed_text name_{};
    std::variant<bool, std::int64_t, borrowed_text> value_{false};
};
struct db_hypertable_options final {
    borrowed_text table_{};
    borrowed_text time_column_{};
    db_expression chunk_interval_{};
    bool create_default_indexes_{true};
    bool if_not_exists_{false};
    bool migrate_data_{false};
};
struct db_compression_order final {
    borrowed_text column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_compression_options final {
    bool enabled_{true};
    std::vector<borrowed_text> segment_by_{};
    std::vector<db_compression_order> order_by_{};
};

// Explicit, versioned schema changes. Each operation consumes its borrowed
// expressions synchronously and stores only generated, owning SQL. Nothing
// here connects to a database or synchronizes a running application's schema.
class db_schema final {
public:
    explicit db_schema(db_schema_options options);
    db_schema(const db_schema&) = delete;
    db_schema& operator=(const db_schema&) = delete;
    db_schema(db_schema&&) noexcept = default;
    db_schema& operator=(db_schema&&) = delete;

    void create_schema(std::string_view name, bool if_not_exists = false);
    void drop_schema(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_table(const db_table_definition& table);
    template <typename entity_type>
    void create_table(const db_entity_table_options& options = {});
    template <typename entity_type>
    void create_relation_tables();
    void drop_table(std::string_view table, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void rename_table(std::string_view table, std::string_view name);
    void add_column(std::string_view table, const db_schema_column& column, bool if_not_exists = false);
    void drop_column(std::string_view table, std::string_view column, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void rename_column(std::string_view table, std::string_view column, std::string_view name);
    void alter_column_type(std::string_view table, std::string_view column, const db_type_definition& type, db_expression using_value = {});
    void set_column_nullable(std::string_view table, std::string_view column, bool nullable);
    void set_column_default(std::string_view table, std::string_view column, db_expression expression);
    void drop_column_default(std::string_view table, std::string_view column);
    void add_constraint(std::string_view table, const db_schema_constraint& constraint);
    void drop_constraint(std::string_view table, std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void validate_constraint(std::string_view table, std::string_view name);
    void create_index(const db_index_definition& index);
    void drop_index(std::string_view name, bool concurrently = false, bool if_exists = false);
    void create_enum(std::string_view name, std::span<const std::string_view> values);
    void add_enum_value(std::string_view name, std::string_view value, bool if_not_exists = false);
    void drop_enum(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_extension(std::string_view name, bool if_not_exists = true);
    void create_view(std::string_view name, const db_query& query, bool replace = false, bool materialized = false);
    void drop_view(std::string_view name, bool materialized = false, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void execute(const db_query& query);
    void perform(db_expression expression);
    void run(const db_procedure& procedure);
    void create_trigger_function(std::string_view name, const db_procedure& procedure, bool replace = false);
    void drop_trigger_function(std::string_view name, db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void create_trigger(const db_trigger_definition& trigger);
    void drop_trigger(std::string_view table, std::string_view name, bool if_exists = false);
    void create_function(const db_function_definition& function);
    void create_function(const db_function_definition& function, const db_procedure& body);
    void create_function(const db_function_definition& function, const db_query& sql_body);
    void drop_function(std::string_view name, std::span<const db_type_definition> parameter_types = {},
        db_drop_behavior behavior = db_drop_behavior::restrict, bool if_exists = false);
    void lock_table(std::string_view table, db_table_lock_mode mode = db_table_lock_mode::access_exclusive, bool nowait = false);
    void set_table_options(std::string_view table, std::span<const db_table_option> options);
    void set_database_option(std::string_view database, std::string_view name, std::string_view value);
    void create_hypertable(const db_hypertable_options& options);
    void set_chunk_time_interval(std::string_view table, db_expression interval);
    void set_compression(std::string_view table, const db_compression_options& options);
    void add_compression_policy(std::string_view table, db_expression after, bool if_not_exists = false);
    void remove_compression_policy(std::string_view table, bool if_exists = false);
    void add_retention_policy(std::string_view table, db_expression after, bool if_not_exists = false);
    void remove_retention_policy(std::string_view table, bool if_exists = false);

    // PostgreSQL batches are one atomic DO statement. A nontransactional
    // operation must be the batch's only operation. MariaDB DDL has implicit
    // commits, so each generated statement receives a stable numbered id.
    [[nodiscard]] std::vector<db_migration> compile(std::string_view id) const;
    [[nodiscard]] db_driver driver() const noexcept {
        return driver_;
    }

private:
    friend class db_procedure;
    struct statement_type final {
        std::string sql_;
        db_migration_atomicity atomicity_{db_migration_atomicity::transactional};
        bool procedural_{false};
    };
    void append(std::string sql, db_migration_atomicity atomicity = db_migration_atomicity::transactional, bool procedural = false);
    void require_postgresql() const;
    void append_type(std::string& sql, const db_type_definition& type) const;
    void append_column(std::string& sql, const db_schema_column& column) const;
    void append_constraint(std::string& sql, const db_schema_constraint& constraint) const;
    void append_expression(std::string& sql, db_expression expression) const;
    void append_drop_behavior(std::string& sql, db_drop_behavior behavior) const;
    [[nodiscard]] std::string constraint_name(std::string_view table, std::string_view relation, std::string_view suffix) const;
    template <typename column_type>
    static db_type_definition entity_column_type() {
        db_type_definition type{.data_type_ = column_type::data_type, .length_ = column_type::options.length_, .precision_ = column_type::options.precision_, .scale_ = column_type::options.scale_};
        if constexpr (detail::is_std_vector<typename column_type::value_type>::value) {
            using item_type = typename detail::is_std_vector<typename column_type::value_type>::value_type;
            if constexpr (column_type::options.data_type_ == db_data_type::inferred || column_type::options.data_type_ == db_data_type::array) {
                type.data_type_ = detail::db_entity_type_traits<item_type>::data_type;
            }
            type.array_ = true;
        }
        if constexpr (!column_type::options.enum_name_.view().empty()) {
            type.custom_name_ = column_type::options.enum_name_.view();
            type.data_type_ = db_data_type::inferred;
        }
        return type;
    }

    db_driver driver_;
    std::vector<statement_type> statements_;
};

template <typename entity_type>
void db_schema::create_table(const db_entity_table_options& options) {
    db_table_definition table_value{.name_ = entity_type::table_name(), .columns_ = std::vector<db_schema_column>{}, .constraints_ = std::vector<db_schema_constraint>{}, .if_not_exists_ = options.if_not_exists_, .temporary_ = options.temporary_, .unlogged_ = options.unlogged_};
    table_value.columns_.reserve(std::tuple_size_v<typename entity_type::columns_type>);
    table_value.constraints_.reserve(options.constraints_.size() + 1U + std::tuple_size_v<typename entity_type::relations_type> * 2U);
    table_value.constraints_.assign(options.constraints_.begin(), options.constraints_.end());
    db_query defaults;
    std::vector<borrowed_text> primary;
    primary.reserve(detail::db_primary_key_count<entity_type>());
    detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
        db_schema_column column{.name_ = c_type::name.view(),
            .type_ = entity_column_type<c_type>(),
            .nullable_ = c_type::options.nullable_,
            .generated_type_ = c_type::options.generated_type_};
        if constexpr (!c_type::options.enum_name_.view().empty()) {
            if (driver_ != db_driver::postgresql) {
                throw std::invalid_argument("entity enum_name requires PostgreSQL");
            }
        }
        if constexpr (!c_type::options.default_expression_.view().empty()) {
            column.default_value_ = defaults.sql(c_type::options.default_expression_.view());
        } else if constexpr (c_type::options.create_date_ || c_type::options.update_date_) {
            column.default_value_ = defaults.call("now");
        } else if constexpr (c_type::options.version_) {
            column.default_value_ = defaults.value(1);
        }
        if constexpr (c_type::options.primary_key_) {
            primary.push_back(column.name_);
        }
        if constexpr (c_type::options.generated_) {
            column.identity_ = db_identity::by_default;
        }
        for (const auto& value : options.generated_columns_) {
            if (value.column_ == column.name_) {
                if (!column.as_expression_.empty()) {
                    throw std::invalid_argument("duplicate entity generated column");
                }
                column.as_expression_ = value.expression_;
            }
        }
        for (const auto& value : options.defaults_) {
            if (value.column_ == column.name_) {
                if (!column.default_value_.empty()) {
                    throw std::invalid_argument("duplicate entity column default");
                }
                column.default_value_ = value.value_;
            }
        }
        table_value.columns_.push_back(std::move(column));
    });
    for (const auto& value : options.generated_columns_) {
        bool found = false;
        for (const auto& column : table_value.columns_) {
            if (column.name_ == value.column_) {
                found = true;
                if (value.expression_.empty()) {
                    throw std::invalid_argument("entity generated column requires an expression");
                }
                if (column.generated_type_ == db_generated_type::none) {
                    throw std::invalid_argument("entity generated column requires generated metadata");
                }
            }
        }
        if (!found) {
            throw std::invalid_argument("entity generated column requires a known column");
        }
    }
    for (auto& column : table_value.columns_) {
        if (column.generated_type_ != db_generated_type::none) {
            if (column.as_expression_.empty()) {
                throw std::invalid_argument("generated column requires an expression");
            }
            if (column.identity_ != db_identity::none || !column.default_value_.empty()) {
                throw std::invalid_argument("generated column conflicts with identity or default");
            }
        }
    }
    for (const auto& value : options.defaults_) {
        bool found = false;
        for (const auto& column : table_value.columns_) {
            found |= column.name_ == value.column_;
        }
        if (!found || value.value_.empty()) {
            throw std::invalid_argument("entity default requires a known column and an expression");
        }
    }
    // Constraints borrow their names, so the reserve keeps these strings in place.
    std::vector<std::string> generated_names;
    generated_names.reserve(1 + std::tuple_size_v<typename entity_type::relations_type> * 2);
    if (!primary.empty()) {
        const auto& pkey_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), {}, "pkey"));
        table_value.constraints_.push_back({.name_ = pkey_name, .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary), .referenced_columns_ = std::vector<borrowed_text>{}});
    }
    detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (!r_type::is_collection && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using mapping_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            const auto& fkey_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), r_type::name.view(), "fkey"));
            db_schema_constraint fk{.name_ = fkey_name, .kind_ = db_constraint_kind::foreign_key, .columns_ = std::vector<borrowed_text>{}, .referenced_table_ = target_type::table_name(), .referenced_columns_ = std::vector<borrowed_text>{}};
            constexpr std::size_t join_col_count = std::tuple_size_v<typename mapping_type::join_columns_type>;
            fk.columns_.reserve(join_col_count);
            fk.referenced_columns_.reserve(join_col_count);
            detail::for_each_descriptor<typename mapping_type::join_columns_type>([&]<typename j_type, std::size_t> { fk.columns_.emplace_back(j_type::local.view()); fk.referenced_columns_.emplace_back(j_type::referenced.view()); });
            table_value.constraints_.push_back(std::move(fk));
            if constexpr (r_type::kind == db_relation_kind::one_to_one) {
                const auto& key_name = generated_names.emplace_back(constraint_name(entity_type::table_name(), r_type::name.view(), "key"));
                table_value.constraints_.push_back({.name_ = key_name, .kind_ = db_constraint_kind::unique, .columns_ = table_value.constraints_.back().columns_, .referenced_columns_ = std::vector<borrowed_text>{}});
            }
        }
    });
    create_table(table_value);
}

template <typename entity_type>
void db_schema::create_relation_tables() {
    detail::for_each_descriptor<typename entity_type::relations_type>([&]<typename r_type, std::size_t> {
        if constexpr (r_type::kind == db_relation_kind::many_to_many && r_type::is_owning) {
            detail::validate_db_relation<entity_type, r_type>();
            using m_type = detail::db_relation_mapping<entity_type, r_type>;
            using target_type = typename r_type::target_entity_type;
            db_table_definition table_value{.name_ = m_type::table_name(), .columns_ = std::vector<db_schema_column>{}, .constraints_ = std::vector<db_schema_constraint>{}};
            table_value.columns_.reserve(std::tuple_size_v<typename m_type::source_columns_type> + std::tuple_size_v<typename m_type::target_columns_type>);
            table_value.constraints_.reserve(3U);
            const auto owner_fk_name = constraint_name(m_type::table_name(), {}, "owner_fkey");
            const auto inverse_fk_name = constraint_name(m_type::table_name(), {}, "inverse_fkey");
            const auto pkey_name = constraint_name(m_type::table_name(), {}, "pkey");
            const auto add_side = [&]<typename referenced_entity_type, typename columns_type>(const std::string& fk_name) {
                db_schema_constraint fk{.name_ = fk_name,
                    .kind_ = db_constraint_kind::foreign_key,
                    .columns_ = std::vector<borrowed_text>{},
                    .referenced_table_ = referenced_entity_type::table_name(),
                    .referenced_columns_ = std::vector<borrowed_text>{}};
                constexpr std::size_t side_count = std::tuple_size_v<columns_type>;
                fk.columns_.reserve(side_count);
                fk.referenced_columns_.reserve(side_count);
                detail::for_each_descriptor<columns_type>([&]<typename j_type, std::size_t> {
                    using c_type = std::tuple_element_t<referenced_entity_type::template column_index<j_type::referenced>(), typename referenced_entity_type::columns_type>;
                    table_value.columns_.push_back({.name_ = j_type::local.view(), .type_ = entity_column_type<c_type>(), .nullable_ = false});
                    fk.columns_.emplace_back(j_type::local.view());
                    fk.referenced_columns_.emplace_back(j_type::referenced.view());
                });
                table_value.constraints_.push_back(std::move(fk));
            };
            add_side.template operator()<entity_type, typename m_type::source_columns_type>(owner_fk_name);
            add_side.template operator()<target_type, typename m_type::target_columns_type>(inverse_fk_name);
            std::vector<borrowed_text> primary;
            primary.reserve(table_value.columns_.size());
            for (const auto& column : table_value.columns_) {
                primary.push_back(column.name_);
            }
            table_value.constraints_.push_back({.name_ = pkey_name, .kind_ = db_constraint_kind::primary_key, .columns_ = std::move(primary)});
            create_table(table_value);
        }
    });
}

template <typename entity_type, typename executor_type>
class db_write_query_builder;

struct db_find_order final {
    borrowed_text column_{};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};

// span 及元素中的文本均为借用；repository 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_find_options final {
    borrowed_span<borrowed_text> select_{};
    db_predicate where_{};
    borrowed_span<borrowed_text> relations_{};
    borrowed_span<db_find_order> order_{};
    std::optional<query_row_count> skip_{};
    std::optional<query_row_count> take_{};
    std::optional<db_lock_options> lock_{};
    db_cache_setting_type cache_{};
    bool with_deleted_{false};
};

// span 及元素中的文本均为借用；upsert 调用入口同步复制或导入。
// 独立保存 options 时，数组及文本 owner 必须存活到入口消费完成。
struct db_upsert_options final {
    borrowed_span<borrowed_text> conflict_paths_{};
    borrowed_span<borrowed_text> update_columns_{};
    bool do_nothing_{false};
    bool any_unique_key_{false};
    bool skip_update_if_no_values_changed_{false};
    db_predicate index_predicate_{};
    borrowed_span<db_assignment> update_expressions_{};
    db_predicate update_where_{};
};

namespace detail {

// This plan belongs to a builder or a single cold operation. It owns every
// selected path and result-column name; it never retains an AST or row view.
class db_relation_plan final {
public:
    static constexpr std::size_t root = std::numeric_limits<std::size_t>::max();
    struct node_type final {
        node_type() = default;
        std::size_t parent_{root};
        std::size_t relation_{0};
        std::size_t offset_{0};
        db_join_type join_{db_join_type::left};
        std::string path_;
        std::string alias_;
        std::vector<std::string> columns_;
    };

    db_relation_plan() = default;
    db_relation_plan(const db_relation_plan&) = default;
    db_relation_plan& operator=(const db_relation_plan&) = delete;
    db_relation_plan(db_relation_plan&&) noexcept = default;
    db_relation_plan& operator=(db_relation_plan&&) = delete;

    [[nodiscard]] db_relation_plan clone() const {
        return *this;
    }
    [[nodiscard]] bool empty() const noexcept {
        return nodes_.empty();
    }
    [[nodiscard]] std::size_t column_count() const noexcept {
        return column_count_;
    }
    [[nodiscard]] std::span<const node_type> nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] std::size_t find(std::size_t parent_value, std::size_t relation) const noexcept;

    template <typename entity_type>
    void add(db_query& query, std::string_view root_alias, std::string_view path,
        std::string_view alias = {}, db_join_type join = db_join_type::left, db_expression condition = {}) {
        if constexpr (db_primary_key_count<entity_type>() == 0) {
            throw std::invalid_argument("relation loading requires a primary key on the root entity");
        } else {
            if (alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("a relation join alias must be one identifier");
            }
            // normalize() reads a leading component as the root alias or a
            // loaded relation's alias; reject paths where it also names a
            // root relation, so the requested relation is never replaced.
            if (const auto dot = path.find('.'); dot != std::string_view::npos) {
                const auto head = path.substr(0, dot);
                bool alias_reading = head == root_alias;
                for (const auto& node : nodes_) {
                    alias_reading |= node.alias_ == head && node.path_ != head;
                }
                if (alias_reading && names_relation<entity_type>(head)) {
                    throw std::invalid_argument("relation path is ambiguous between an alias and a relation name");
                }
            }
            auto normalized = normalize(root_alias, path);
            validate_path<entity_type>(normalized, 0);
            if (!alias.empty() && uses_source_name(query, alias)) {
                bool same = false;
                for (const auto& node : nodes_) {
                    same |= node.path_ == normalized && node.alias_ == alias && node.join_ == join && condition.empty();
                }
                if (!same) {
                    throw std::invalid_argument("relation join alias is already in use");
                }
            }
            if (nodes_.empty()) {
                column_count_ = std::tuple_size_v<typename entity_type::columns_type>;
            }
            add_path<entity_type>(query, root_alias, normalized, root, alias, join, condition);
        }
    }

    template <typename entity_type>
    static void join_relation(db_query& query, std::string_view root_alias, std::string_view relation_path,
        std::string_view alias, db_join_type join, db_expression condition = {}) {
        if (alias.empty() || alias.find('.') != std::string_view::npos || alias.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("a relation join alias must be one identifier");
        }
        auto source_alias = root_alias;
        auto relation_name = relation_path;
        if (const auto dot = relation_path.find('.'); dot != std::string_view::npos) {
            source_alias = relation_path.substr(0, dot);
            relation_name = relation_path.substr(dot + 1);
        }
        if (source_alias.empty() || relation_name.empty() || relation_name.find('.') != std::string_view::npos) {
            throw std::invalid_argument("relation join path requires a direct relation or alias.relation");
        }
        if (source_alias != root_alias) {
            throw std::invalid_argument("unknown database entity relation");
        }
        if (uses_source_name(query, alias)) {
            throw std::invalid_argument("relation join alias is already in use");
        }
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (relation_name != relation_type::name.view()) {
                return;
            }
            found = true;
            validate_db_relation<entity_type, relation_type>();
            using mapping_type = db_relation_mapping<entity_type, relation_type>;
            if constexpr (mapping_type::through_join_table) {
                std::string junction;
                junction.reserve(alias.size() + 9U);
                junction.append(alias);
                junction.append("_junction");
                if (uses_source_name(query, junction)) {
                    throw std::invalid_argument("relation junction alias is already in use");
                }
                append_relation_joins<entity_type, relation_type>(
                    query, source_alias, alias, junction, join, condition);
            } else {
                append_relation_joins<entity_type, relation_type>(
                    query, source_alias, alias, {}, join, condition);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    template <typename entity_type>
    [[nodiscard]] std::optional<db_query> prepare(const db_query& query,
        std::string_view root_alias, db_driver driver) const {
        constexpr auto keys = db_primary_key_columns<entity_type>();
        return prepare_entity_read(query, keys, root_alias, driver);
    }

private:
    [[nodiscard]] std::string normalize(std::string_view root_alias, std::string_view path) const;
    [[nodiscard]] static bool uses_source_name(const db_query& query, std::string_view name);
    [[nodiscard]] static bool uses_projection_name(const db_query& query, std::string_view name);
    // Returns the rewritten query, or nullopt when the original query can
    // load relations unchanged.
    [[nodiscard]] static std::optional<db_query> prepare_entity_read(const db_query& query,
        std::span<const std::string_view> primary_key, std::string_view root_alias, db_driver driver);

    template <typename entity_type>
    [[nodiscard]] static bool names_relation(std::string_view name) {
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            found = found || name == relation_type::name.view();
        });
        return found;
    }

    template <typename entity_type>
    static void validate_path(std::string_view path, std::size_t depth) {
        if (path.empty() || depth >= max_db_query_depth) {
            throw std::invalid_argument("relation path is empty or exceeds query nesting depth");
        }
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        if (name.empty() || (dot != std::string_view::npos && rest.empty())) {
            throw std::invalid_argument("relation path contains an empty component");
        }
        bool found = false;
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t> {
            if (name != relation_type::name.view()) {
                return;
            }
            found = true;
            validate_db_relation<entity_type, relation_type>();
            using target_type = typename relation_type::target_entity_type;
            if constexpr (db_primary_key_count<target_type>() == 0) {
                throw std::invalid_argument("relation loading requires a primary key on every selected target");
            } else if (!rest.empty()) {
                validate_path<target_type>(rest, depth + 1);
            }
        });
        if (!found) {
            throw std::invalid_argument("unknown database entity relation");
        }
    }

    [[nodiscard]] std::string unique_alias(const db_query& query, std::string_view reserved);
    [[nodiscard]] std::string unique_projection(const db_query& query);
    static void conjunction(db_query& query, db_expression& predicate, db_expression term);

    template <typename entity_type, typename relation_type>
    static void append_relation_joins(db_query& query, std::string_view source_alias,
        std::string_view target_alias, std::string_view junction_alias, db_join_type join,
        db_expression extra_condition) {
        using mapping_type = db_relation_mapping<entity_type, relation_type>;
        using target_type = typename relation_type::target_entity_type;
        db_expression predicate;
        if constexpr (mapping_type::through_join_table) {
            for_each_descriptor<typename mapping_type::source_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::referenced.view(), source_alias),
                        db_binary_operator::equal, query.column(column_type::local.view(), junction_alias)));
            });
            query.join(join, mapping_type::table_name(), predicate, junction_alias);
            predicate = {};
            for_each_descriptor<typename mapping_type::target_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::local.view(), junction_alias),
                        db_binary_operator::equal, query.column(column_type::referenced.view(), target_alias)));
            });
        } else {
            (void)junction_alias;
            for_each_descriptor<typename mapping_type::join_columns_type>([&]<typename column_type, std::size_t> {
                conjunction(query, predicate,
                    query.binary(query.column(column_type::local.view(), source_alias),
                        db_binary_operator::equal, query.column(column_type::referenced.view(), target_alias)));
            });
        }
        if (!extra_condition.empty()) {
            conjunction(query, predicate, query.import_expression(extra_condition));
        }
        query.join(join, target_type::table_name(), predicate, target_alias);
    }

    template <typename entity_type, typename relation_type>
    void append_join(db_query& query, std::string_view source_alias, node_type& node_value,
        std::string_view reserved_alias, db_expression extra_condition) {
        using mapping_type = db_relation_mapping<entity_type, relation_type>;
        using target_type = typename relation_type::target_entity_type;
        if constexpr (mapping_type::through_join_table) {
            auto junction = unique_alias(query, node_value.alias_);
            // A terminal user alias is reserved even while constructing an
            // ancestor's automatic junction alias.
            if (junction == reserved_alias) {
                junction = unique_alias(query, reserved_alias);
            }
            append_relation_joins<entity_type, relation_type>(
                query, source_alias, node_value.alias_, junction, node_value.join_, extra_condition);
        } else {
            (void)reserved_alias;
            append_relation_joins<entity_type, relation_type>(
                query, source_alias, node_value.alias_, {}, node_value.join_, extra_condition);
        }
        node_value.columns_.reserve(std::tuple_size_v<typename target_type::columns_type>);
        for_each_descriptor<typename target_type::columns_type>([&]<typename column_type, std::size_t> {
            auto name = unique_projection(query);
            query.add_select(query.alias(query.column(column_type::name.view(), node_value.alias_), name));
            node_value.columns_.push_back(std::move(name));
        });
    }

    template <typename entity_type>
    void add_path(db_query& query, std::string_view root_alias, std::string_view path,
        std::size_t parent_value, std::string_view requested_alias, db_join_type requested_join, db_expression condition) {
        const auto dot = path.find('.');
        const auto name = path.substr(0, dot);
        const auto rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            if (name != relation_type::name.view()) {
                return;
            }
            auto index = find(parent_value, i);
            if (index == root) {
                node_type node;
                node.parent_ = parent_value;
                node.relation_ = i;
                node.offset_ = column_count_;
                node.join_ = rest.empty() ? requested_join : db_join_type::left;
                if (parent_value != root) {
                    node.path_.reserve(nodes_[parent_value].path_.size() + 1U + name.size());
                    node.path_.append(nodes_[parent_value].path_);
                    node.path_.push_back('.');
                }
                node.path_.append(name);
                node.alias_ = rest.empty() && !requested_alias.empty()
                                  ? std::string(requested_alias)
                                  : unique_alias(query, requested_alias);
                const auto source_alias = parent_value == root ? root_alias : std::string_view(nodes_[parent_value].alias_);
                append_join<entity_type, relation_type>(
                    query, source_alias, node, requested_alias, rest.empty() ? condition : db_expression{});
                column_count_ += node.columns_.size();
                index = nodes_.size();
                nodes_.push_back(std::move(node));
            } else if (rest.empty() && ((!requested_alias.empty() && nodes_[index].alias_ != requested_alias) || nodes_[index].join_ != requested_join || !condition.empty())) {
                throw std::invalid_argument("relation path was already loaded with another alias or join kind");
            }
            if (!rest.empty()) {
                add_path<typename relation_type::target_entity_type>(
                    query, root_alias, rest, index, requested_alias, requested_join, condition);
            }
        });
    }

    std::vector<node_type> nodes_;
    std::size_t column_count_{0};
    std::size_t next_alias_{0};
    std::size_t next_projection_{0};
};

struct db_graph_identity final {
    std::size_t index_{0};
    std::size_t scope_{0};
};

struct db_graph_key final {
    std::size_t owner_{0};
    std::size_t relation_{0};
    std::string_view pk_{};

    bool operator==(const db_graph_key&) const noexcept = default;
};

struct db_graph_key_hash final {
    [[nodiscard]] std::size_t operator()(const db_graph_key& key) const noexcept {
        std::size_t seed = std::hash<std::size_t>{}(key.owner_);
        seed ^= std::hash<std::size_t>{}(key.relation_) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) + (seed >> 2U);
        seed ^= std::hash<std::string_view>{}(key.pk_) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) + (seed >> 2U);
        return seed;
    }
};

// Indices, rather than pointers into result vectors, remain valid when a
// collection grows. The identity table is temporary operation storage; the
// resulting entities own their nested values without retaining this table.
class db_relation_decoder final {
public:
    explicit db_relation_decoder(const db_relation_plan& plan)
        : plan_(plan) {}

    template <typename entity_type>
    entity_rows<entity_type> decode(const db_rows& rows) {
        entity_rows<entity_type> result;
        if (rows.empty()) {
            return result;
        }
        resolve_columns<entity_type>(rows.front());
        identities_.reserve(rows.size());
        for (const auto& row : rows) {
            const auto key = make_key<entity_type>(row, 0, 0, db_relation_plan::root);
            if (!key) {
                throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "NULL root primary key in a relation query");
            }
            auto position = identities_.find(*key);
            db_graph_identity identity;
            if (position == identities_.end()) {
                identity = {result.size(), next_scope_++};
                entity_type entity;
                decode_columns(entity, row, 0);
                result.push_back(std::move(entity));
                store_identity<entity_type>(*key, identity);
            } else {
                identity = position->second;
            }
            decode_relations(result[identity.index_], row, db_relation_plan::root, identity.scope_);
        }
        return result;
    }

private:
    template <typename entity_type>
    void resolve_columns(const db_row& row) {
        columns_.resize(plan_.column_count());
        const auto names = row.column_names();
        const auto resolve = [&](std::string_view name) {
            const auto lookup = find_unique_db_result_column(names, name);
            if (lookup.state_ == unique_lookup_status::repeated) {
                throw db_conversion_error(db_conversion_error::code_type::invalid_format, "duplicate entity projection name in a relation query");
            }
            if (lookup.state_ == unique_lookup_status::absent || lookup.index_ >= row.size()) {
                throw db_conversion_error(db_conversion_error::code_type::missing_column, "relation query must select every mapped entity column");
            }
            return lookup.index_;
        };
        for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            columns_[i] = resolve(column_type::name.view());
        });
        for (const auto& node : plan_.nodes()) {
            for (std::size_t i = 0; i < node.columns_.size(); ++i) {
                columns_[node.offset_ + i] = resolve(node.columns_[i]);
            }
        }
    }

    template <typename entity_type>
    void decode_columns(entity_type& entity, const db_row& row, std::size_t offset) const {
        for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
            decode_entity_field<entity_type, column_type>(entity, row[columns_[offset + i]]);
        });
    }

    template <typename entity_type>
    std::optional<db_graph_key> make_key(const db_row& row, std::size_t offset, std::size_t owner_value, std::size_t relation) {
        if constexpr (db_primary_key_count<entity_type>() == 1) {
            std::optional<std::string_view> pk;
            for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
                if constexpr (column_type::options.primary_key_) {
                    pk = row[columns_[offset + i]].value();
                }
            });
            if (!pk) {
                return std::nullopt;
            }
            return db_graph_key{owner_value, relation, *pk};
        } else {
            key_.clear();
            if (key_.capacity() < 64) {
                key_.reserve(64);
            }
            std::size_t nulls = 0;
            for_each_descriptor<typename entity_type::columns_type>([&]<typename column_type, std::size_t i> {
                if constexpr (column_type::options.primary_key_) {
                    const auto value = row[columns_[offset + i]].value();
                    if (!value) {
                        ++nulls;
                        return;
                    }
                    append_db_number(key_, static_cast<std::uint64_t>(value->size()));
                    key_.push_back(':');
                    key_.append(*value);
                }
            });
            if (nulls != 0 && nulls != db_primary_key_count<entity_type>()) {
                throw db_conversion_error(db_conversion_error::code_type::unexpected_null, "partially NULL composite primary key in a relation query");
            }
            if (nulls != 0) {
                return std::nullopt;
            }
            return db_graph_key{owner_value, relation, key_};
        }
    }

    template <typename entity_type>
    void store_identity(db_graph_key key, db_graph_identity identity) {
        if constexpr (db_primary_key_count<entity_type>() > 1) {
            key.pk_ = composite_keys_.emplace_back(key.pk_);
        }
        identities_.emplace(key, identity);
    }

    template <typename entity_type>
    void decode_relations(entity_type& entity, const db_row& row, std::size_t parent_value, std::size_t scope) {
        for_each_descriptor<typename entity_type::relations_type>([&]<typename relation_type, std::size_t i> {
            const auto node_index = plan_.find(parent_value, i);
            if (node_index == db_relation_plan::root) {
                return;
            }
            const auto& node_value = plan_.nodes()[node_index];
            using target_type = typename relation_type::target_entity_type;
            const auto key = make_key<target_type>(row, node_value.offset_, scope, node_index);
            if constexpr (relation_type::is_collection) {
                auto& collection_value = db_entity_access<entity_type>::template ensure_relation_collection<relation_type::name>(entity);
                if (!key) {
                    return;
                }
                auto position = identities_.find(*key);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    identity = {collection_value.size(), next_scope_++};
                    target_type child_value;
                    decode_columns(child_value, row, node_value.offset_);
                    collection_value.push_back(std::move(child_value));
                    store_identity<target_type>(*key, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(collection_value[identity.index_], row, node_index, identity.scope_);
            } else {
                if (!key) {
                    if (entity.template is_set<relation_type::name>() && !entity.template is_null<relation_type::name>()) {
                        throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "inconsistent to-one rows in a relation query");
                    }
                    db_entity_access<entity_type>::template set_relation_null<relation_type::name>(entity);
                    return;
                }
                auto position = identities_.find(*key);
                db_graph_identity identity;
                if (position == identities_.end()) {
                    if (entity.template is_set<relation_type::name>()) {
                        throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "to-one relation returned more than one target");
                    }
                    identity = {0, next_scope_++};
                    auto& child_value = db_entity_access<entity_type>::template emplace_relation<relation_type::name>(entity);
                    decode_columns(child_value, row, node_value.offset_);
                    store_identity<target_type>(*key, identity);
                } else {
                    identity = position->second;
                }
                decode_relations(entity.template get<relation_type::name>(), row, node_index, identity.scope_);
            }
        });
    }

    const db_relation_plan& plan_;
    std::vector<std::size_t> columns_;
    std::unordered_map<db_graph_key, db_graph_identity, db_graph_key_hash> identities_;
    std::deque<std::string> composite_keys_;
    std::string key_;
    std::size_t next_scope_{1};
};

template <typename entity_type>
struct db_map_related_entities final {
    db_relation_plan plan_;
    entity_rows<entity_type> operator()(db_rows&& rows) {
        return db_relation_decoder(plan_).template decode<entity_type>(rows);
    }
};

template <typename entity_type>
struct db_map_one_related_entity final {
    db_relation_plan plan_;
    std::optional<entity_type> operator()(db_rows&& rows) {
        auto entities = db_relation_decoder(plan_).template decode<entity_type>(rows);
        if (entities.empty()) {
            return std::nullopt;
        }
        if (entities.size() != 1) {
            throw db_conversion_error(db_conversion_error::code_type::unexpected_row_count, "single-entity relation query returned multiple roots");
        }
        return std::optional<entity_type>(std::in_place, std::move(entities[0]));
    }
};

// Imports each source into `query` and passes the imported expressions to
// `consume` in one call; small lists stay on the stack.
template <typename source_type, typename import_type, typename consume_type>
void import_db_expressions([[maybe_unused]] db_query& query, std::span<const source_type> sources, import_type&& import_one, consume_type&& consume) {
    static constexpr std::size_t inline_capacity = 16;
    if (sources.size() <= inline_capacity) {
        std::array<db_expression, inline_capacity> imported{};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            imported[i] = import_one(sources[i]);
        }
        consume(std::span<const db_expression>(imported.data(), sources.size()));
        return;
    }
    std::vector<db_expression> imported;
    imported.reserve(sources.size());
    for (const auto& source : sources) {
        imported.push_back(import_one(source));
    }
    consume(std::span<const db_expression>(imported));
}

// Stages borrowed column names as one db_selection list for `consume`.
template <std::size_t inline_capacity, typename name_type, typename consume_type>
decltype(auto) stage_db_selections(std::span<const name_type> columns, consume_type&& consume) {
    if (columns.size() <= inline_capacity) {
        std::array<db_selection, inline_capacity> selections{};
        for (std::size_t i = 0; i < columns.size(); ++i) {
            selections[i] = db_selection{columns[i]};
        }
        return consume(std::span<const db_selection>(selections.data(), columns.size()));
    }
    std::vector<db_selection> selections;
    selections.reserve(columns.size());
    for (const auto& column : columns) {
        selections.emplace_back(column);
    }
    return consume(std::span<const db_selection>(selections));
}

}  // namespace detail

class db_values_builder final {
public:
    explicit db_values_builder(std::span<const std::string_view> columns) {
        if (columns.empty()) {
            throw std::invalid_argument("db_values_builder requires at least one column");
        }
        columns_.reserve(columns.size());
        for (const auto column : columns) {
            columns_.emplace_back(column);
        }
    }
    explicit db_values_builder(std::initializer_list<std::string_view> columns)
        : db_values_builder(std::span<const std::string_view>(columns.begin(), columns.size())) {}

    db_values_builder& row(std::span<const db_expr> values) {
        if (values.size() != columns_.size()) {
            throw std::invalid_argument("db_values_builder row width differs from column count");
        }
        detail::import_db_expressions(
            query_, values, [&](const db_expr& value) { return value.expression(query_); },
            [&](std::span<const db_expression> imported) { query_.values(imported); });
        return *this;
    }
    db_values_builder& row(std::initializer_list<db_expr> values) {
        return row(std::span<const db_expr>(values.begin(), values.size()));
    }

    [[nodiscard]] const db_query& query() const& noexcept RUVIA_LIFETIMEBOUND {
        return query_;
    }
    const db_query& query() const&& = delete;
    [[nodiscard]] const std::vector<std::string>& columns() const& noexcept RUVIA_LIFETIMEBOUND {
        return columns_;
    }
    const std::vector<std::string>& columns() const&& = delete;

private:
    db_query query_;
    std::vector<std::string> columns_;
};

namespace detail {

// Runtime column names resolve against one descriptor table per entity.
struct db_column_info final {
    std::string_view name_;
    bool writable_{false};
    bool nullable_{false};
    bool numeric_{false};
    bool primary_key_{false};
    bool conflict_updatable_{false};
};
template <typename e_type>
inline constexpr auto db_entity_columns = []<typename... columns_type>(std::tuple<columns_type...>*) {
    return std::array<db_column_info, sizeof...(columns_type)>{db_column_info{
        columns_type::name.view(), is_writable_db_column<columns_type>(), columns_type::options.nullable_, is_numeric_db_column<columns_type>(),
        columns_type::options.primary_key_, is_conflict_updatable_db_column<columns_type>()}...};
}(static_cast<typename e_type::columns_type*>(nullptr));
const db_column_info& require_db_column(std::span<const db_column_info> columns, std::string_view name);
void validate_insert_columns(std::span<const db_column_info> entity_columns, std::span<const std::string_view> columns);
// Lifecycle columns are writable by the db_column invariant.
template <typename e_type>
consteval std::string_view delete_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.delete_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view update_date_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.update_date_) {
            result = c_type::name.view();
        }
    });
    return result;
}
template <typename e_type>
consteval std::string_view version_column() noexcept {
    std::string_view result{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (c_type::options.version_) {
            result = c_type::name.view();
        }
    });
    return result;
}
// The one lifecycle policy for ORM-generated UPDATE and conflict-update
// assignments: update_date_ takes now() and version_ increments by one, unless
// the statement assigns that column itself (`assigned`).
template <typename assigned_type, typename sink_type>
void append_lifecycle_assignments(db_query& query, std::string_view qualifier,
    std::string_view update_date, std::string_view version, assigned_type&& assigned, sink_type&& sink) {
    if (!update_date.empty()) {
        if (!assigned(update_date)) {
            sink(update_date, query.call("now"));
        }
    }
    if (!version.empty()) {
        if (!assigned(version)) {
            sink(version, query.binary(query.column(version, qualifier), db_binary_operator::add, query.value(1)));
        }
    }
}
// Stamps an UPDATE before any explicit assignment; a later SET of the same
// column replaces the stamp, so explicit values win.
template <typename e_type>
void stamp_lifecycle_columns(db_query& query, std::string_view qualifier) {
    append_lifecycle_assignments(
        query, qualifier, update_date_column<e_type>(), version_column<e_type>(), [](std::string_view) noexcept { return false; },
        [&query](std::string_view column, db_expression value) { query.set(column, value); });
}
// Multi-row conflict updates read EXCLUDED, which holds the column default for
// rows that left a column unset; every auto-updated column must be set in all
// rows or in none.
template <typename e_type>
void require_uniform_set_columns(std::span<const e_type> entities, std::span<const db_assignment> overridden) {
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t> {
        if constexpr (is_conflict_updatable_db_column<c_type>()) {
            if (std::ranges::any_of(overridden, [](const db_assignment& item) { return item.column_ == c_type::name.view(); })) {
                return;
            }
            const auto set_count = static_cast<std::size_t>(std::ranges::count_if(
                entities, [](const e_type& entity) { return entity.template is_set<c_type::name>(); }));
            if (set_count != 0 && set_count != entities.size()) {
                throw std::invalid_argument("bulk upsert requires identical set columns or explicit update_columns");
            }
        }
    });
}
template <typename joined_type, typename e_type>
[[nodiscard]] db_expression import_join_predicate(db_query& query, const db_predicate& on, std::string_view alias, std::string_view join_alias) {
    if (on.empty()) {
        return {};
    }
    auto imported = on.expression(query, e_type::table_name(), alias);
    if (!join_alias.empty() && joined_type::table_name() != e_type::table_name()) {
        imported = query.import_expression(imported, joined_type::table_name(), join_alias);
    }
    return imported;
}
template <typename e_type>
void select_entity(db_query& query, std::string_view alias) {
    const auto qualifier = alias.empty() ? e_type::table_name() : alias;
    std::array<db_expression, std::tuple_size_v<typename e_type::columns_type>> columns{};
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
        columns[index] = query.column(c_type::name.view(), qualifier);
    });
    query.select(columns);
    query.from(e_type::table_name(), alias);
}
template <typename e_type>
void apply_find_options(db_query& query, const db_find_options& options, std::string_view alias = {}) {
    query.cache(options.cache_);
    if (!options.where_.empty()) {
        query.where(options.where_.expression(query, e_type::table_name(), alias));
    }
    for (const auto& order : options.order_) {
        require_db_column(db_entity_columns<e_type>, order.column_);
        query.add_order_by(query.column(order.column_, alias.empty() ? e_type::table_name() : alias), order.direction_, order.nulls_);
    }
    query.offset(options.skip_).limit(options.take_);
    if (options.lock_) {
        query.lock(*options.lock_);
    }
}
struct db_map_count final {
    std::uint64_t operator()(db_rows&& rows) const {
        return db_count_value(rows);
    }
};
struct db_map_exists final {
    bool operator()(db_rows&& rows) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database exists did not return exactly one row");
        }
        const auto value = rows.front()["exists"].template as<bool>();
        if (!value) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_null, "database exists column is NULL");
        }
        return *value;
    }
};
template <typename e_type, typename c_type>
db_expression entity_column_value(db_query& query, const e_type& entity) {
    if (!entity.template is_set<c_type::name>()) {
        return query.default_value();
    }
    if (entity.template is_null<c_type::name>()) {
        return query.null_value();
    }
    return query.value(entity_db_value(entity.template get<c_type::name>()));
}

template <typename e_type>
void append_entity_insert_rows(db_query& query, std::span<const e_type> entities,
    std::array<std::string_view, std::tuple_size_v<typename e_type::columns_type>>& columns,
    std::size_t& column_count) {
    if (entities.empty()) {
        throw std::invalid_argument("repository insert requires an entity");
    }
    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected{};
    column_count = 0;
    for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
        if constexpr (is_writable_db_column<c_type>()) {
            if (std::ranges::any_of(entities, [](const e_type& entity) { return entity.template is_set<c_type::name>(); })) {
                selected[index] = true;
                columns[column_count++] = c_type::name.view();
            }
        }
    });
    std::vector<db_expression> values;
    if (column_count != 0 && entities.size() > values.max_size() / column_count) {
        throw std::length_error("repository insert batch is too large");
    }
    values.reserve(column_count * entities.size());
    for (const auto& entity : entities) {
        for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
            if constexpr (is_writable_db_column<c_type>()) {
                if (selected[index]) {
                    values.push_back(entity_column_value<e_type, c_type>(query, entity));
                }
            }
        });
    }
    query.insert_rows(std::span<const std::string_view>(columns.data(), column_count), values, entities.size());
}

template <typename e_type>
struct db_map_projection final {
    static_assert(sql_entity<e_type> || sql_projection<e_type>, "SQL results require a SQL entity or a db_projection");
    explicit db_map_projection(std::span<const std::string> selected, bool allow_extra_columns = false, bool wildcard = false)
        : wildcard_(wildcard) {
        if (!wildcard && selected.empty()) {
            selected_columns_.fill(true);
            return;
        }
        if (!allow_extra_columns) {
            for (const auto& name : selected) {
                require_db_column(db_entity_columns<e_type>, name);
            }
        }
        for_each_descriptor<typename e_type::columns_type>([&]<typename c_type, std::size_t index> {
            selected_columns_[index] = std::ranges::find(selected, c_type::name.view()) != selected.end();
        });
    }
    entity_rows<e_type> operator()(const db_rows& rows) const {
        auto result_value = db_result_access::make_entity_rows<e_type>(rows.size());
        db_entity_row_decoder<e_type> decoder(selected_columns_, wildcard_);
        for (const auto& row : rows) {
            result_value.push_back(decoder.decode(row));
        }
        return result_value;
    }

    std::array<bool, std::tuple_size_v<typename e_type::columns_type>> selected_columns_{};
    bool wildcard_{false};
};
template <typename e_type>
struct db_map_one_projection final {
    db_map_projection<e_type> mapper_;
    std::optional<e_type> operator()(db_rows&& rows) const {
        if (rows.empty()) {
            return std::nullopt;
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front());
    }
};
struct db_map_raw_rows final {
    db_rows operator()(db_rows&& rows) const noexcept {
        return std::move(rows);
    }
};
template <db_mapped_value scalar_type>
struct db_map_scalar final {
    std::optional<scalar_type> operator()(db_rows&& rows) const {
        if (rows.empty() || !rows.front()[std::size_t{0}].value()) {
            return std::nullopt;
        }
        return ::ruvia::decode_db_field<scalar_type>(rows.front()[std::size_t{0}]);
    }
};
template <db_mapped_value scalar_type>
struct db_map_scalars final {
    std::vector<scalar_type> operator()(db_rows&& rows) const {
        std::vector<scalar_type> result;
        result.reserve(rows.size());
        for (const auto& row : rows) {
            if (!row[std::size_t{0}].value()) {
                throw db_conversion_error(
                    db_conversion_error::code_type::unexpected_null, "database scalar column is NULL");
            }
            result.push_back(::ruvia::decode_db_field<scalar_type>(row[std::size_t{0}]));
        }
        return result;
    }
};
template <typename e_type>
struct db_map_raw_and_entities final {
    db_map_projection<e_type> mapper_;
    std::pair<entity_rows<e_type>, db_rows> operator()(db_rows&& rows) const {
        auto entities = mapper_(rows);
        return {std::move(entities), std::move(rows)};
    }
};
template <typename e_type>
struct db_map_required_one final {
    db_map_projection<e_type> mapper_;
    e_type operator()(db_rows&& rows) const {
        if (rows.size() != 1) {
            throw db_conversion_error(
                db_conversion_error::code_type::unexpected_row_count, "database save did not return the saved row");
        }
        db_entity_row_decoder<e_type> decoder(mapper_.selected_columns_, mapper_.wildcard_);
        return decoder.decode(rows.front());
    }
};
struct db_projection final {
    db_projection() = default;
    [[nodiscard]] bool empty() const noexcept {
        return columns_.empty() && !wildcard_;
    }
    std::vector<std::string> columns_;
    bool wildcard_{false};
};
// Wildcards stay in the AST; only explicit result names are tracked for mapping.
db_projection apply_projection(db_query& query, std::span<const db_selection> selection,
    std::string_view qualifier, bool returning = false, std::string_view root_table = {},
    const db_projection* previous = nullptr);
[[nodiscard]] std::string format_truncate_table(std::string_view table, db_driver driver);

void apply_entity_upsert(db_query& query, std::span<const db_column_info> entity_columns,
    std::span<db_assignment> updates, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view table, std::string_view qualifier,
    std::string_view update_date, std::string_view version);

template <typename entity_type>
void apply_entity_upsert(db_query& query, std::span<const std::string_view> inserted_columns,
    const db_upsert_options& options, db_driver driver, std::string_view qualifier) {
    std::array<db_assignment, std::tuple_size_v<typename entity_type::columns_type>> updates{};
    apply_entity_upsert(query, db_entity_columns<entity_type>, updates, inserted_columns,
        options, driver, entity_type::table_name(), qualifier, update_date_column<entity_type>(), version_column<entity_type>());
}

template <typename executor_type>
using db_repository_executor_type = std::conditional_t<std::same_as<executor_type, db_session>, db_session, executor_type&>;
template <typename executor_type>
using db_repository_input_type = std::conditional_t<std::same_as<executor_type, db_session>, const db_session&, executor_type&>;

}  // namespace detail

template <typename entity_type, typename executor_type>
class db_query_builder final {
public:
    db_query_builder(const db_query_builder&) = delete;
    db_query_builder& operator=(const db_query_builder&) = delete;
    db_query_builder(db_query_builder&&) = default;
    db_query_builder& operator=(db_query_builder&&) = delete;

    db_query_builder& select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        for (const auto& field : fields_value) {
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
            }
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name());
        return *this;
    }
    db_query_builder& select(std::initializer_list<db_selection> fields_value) {
        return select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& select(std::span<const std::string_view> columns) {
        return detail::stage_db_selections<column_capacity>(columns,
            [this](std::span<const db_selection> selections) -> db_query_builder& { return select(selections); });
    }
    db_query_builder& select(const db_expr& expression, borrowed_text alias_name) {
        return select({expression.as(alias_name)});
    }
    db_query_builder& add_select(std::span<const db_selection> fields_value) {
        require_plain_projection();
        if (fields_value.empty()) {
            throw std::invalid_argument("add_select requires at least one field");
        }
        for (const auto& field : fields_value) {
            if (!field.has_expression() && !field.is_wildcard()) {
                detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
            }
        }
        detail::db_projection implicit;
        const auto* previous = &selected_;
        if (selected_.empty()) {
            implicit.columns_.reserve(column_capacity);
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                implicit.columns_.emplace_back(c_type::name.view());
            });
            previous = &implicit;
        }
        selected_ = detail::apply_projection(query_, fields_value, alias_, false, entity_type::table_name(), previous);
        return *this;
    }
    db_query_builder& add_select(std::initializer_list<db_selection> fields_value) {
        return add_select(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_query_builder& add_select(std::string_view column) {
        return add_select({db_selection{column}});
    }
    db_query_builder& add_select(const db_expr& expression, borrowed_text alias_name) {
        return add_select({expression.as(alias_name)});
    }
    db_query_builder& from_nothing() {
        require_plain_projection();
        query_.clear_source();
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), detail::import_join_predicate<joined_type, entity_type>(query_, on, alias_, alias), alias);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::span<const std::string_view> using_columns) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), {}, alias, using_columns);
        return *this;
    }
    template <typename joined_type>
    db_query_builder& join(db_join_type type, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join<joined_type>(type, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    template <typename source, typename source_executor_type>
    db_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, detail::import_join_predicate<source, entity_type>(query_, on, alias_, alias), alias, options);
        return *this;
    }
    db_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::span<const std::string_view> using_columns) {
        query_.join(type, name, {}, alias, using_columns);
        return *this;
    }
    db_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, std::initializer_list<std::string_view> using_columns) {
        return join_cte(type, name, alias, std::span<const std::string_view>(using_columns.begin(), using_columns.size()));
    }
    db_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_query_builder& left_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& inner_join(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        detail::db_relation_plan::template join_relation<entity_type>(
            query_, alias_, relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    template <typename joined_type>
    db_query_builder& left_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::left, alias, on);
    }
    template <typename joined_type>
    db_query_builder& inner_join(std::string_view alias, const db_predicate& on = {}) {
        return join<joined_type>(db_join_type::inner, alias, on);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& left_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& inner_join(const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    db_query_builder& left_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::left, source_value, alias, on, options);
    }
    db_query_builder& inner_join(const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        return join(db_join_type::inner, source_value, alias, on, options);
    }
    template <typename source_type>
        requires requires(const source_type& source) {
            { source.query() } -> std::same_as<const db_query&>;
        }
    db_query_builder& with(std::string_view name, const source_type& source, const db_cte_options& options = {}) {
        return with(name, source.query(), options);
    }
    db_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        if (source_value.has_writes()) {
            require_plain_projection();
        }
        query_.with(name, source_value, options);
        return *this;
    }
    db_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_query_builder& from_cte(std::string_view name) {
        require_plain_projection();
        query_.from(name, alias_);
        return *this;
    }
    db_query_builder& from_function(db_expression function, std::string_view alias = {}, const db_source_options& options = {}) {
        require_plain_projection();
        query_.from_function(query_.import_expression(function), alias.empty() ? std::string_view(alias_) : alias, options);
        return *this;
    }
    // Borrows the flat AST, without applying execution-time soft-delete filtering.
    // The builder and its resource must outlive the view; consumers clone on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        require_plain_projection();
        return query_;
    }
    const db_query& query() const&& = delete;
    db_expression subquery(db_query& expressions) const {
        require_plain_projection();
        return expressions.subquery(query_);
    }
    db_expression exists(db_query& expressions) const {
        require_plain_projection();
        return expressions.exists(query_);
    }
    [[nodiscard]] db_predicate exists() const {
        require_plain_projection();
        return db_predicate::exists(query_);
    }
    [[nodiscard]] db_predicate not_exists() const {
        require_plain_projection();
        return db_predicate::not_exists(query_);
    }
    template <typename source, typename source_executor_type>
    db_query_builder& combine(db_set_operation operation, const db_query_builder<source, source_executor_type>& source_value) {
        require_plain_projection();
        source_value.require_plain_projection();
        query_.combine(operation, source_value.query_);
        return *this;
    }
    db_query_builder& group_by(std::span<const db_expr> expressions) {
        detail::import_db_expressions(query_, expressions, [this](const db_expr& expression) { return import_entity_expression(expression); }, [this](std::span<const db_expression> imported) { query_.group_by(imported); });
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<db_expr> expressions) {
        return group_by(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& group_by(std::span<const std::string_view> columns) {
        detail::import_db_expressions(query_, columns, [this](std::string_view column) { return import_entity_column(column); }, [this](std::span<const db_expression> imported) { query_.group_by(imported); });
        return *this;
    }
    db_query_builder& group_by(std::initializer_list<std::string_view> columns) {
        return group_by(std::span<const std::string_view>(columns.begin(), columns.size()));
    }
    db_query_builder& add_group_by(std::span<const db_expr> expressions) {
        for (const auto& expression : expressions) {
            query_.add_group_by(import_entity_expression(expression));
        }
        return *this;
    }
    db_query_builder& add_group_by(std::initializer_list<db_expr> expressions) {
        return add_group_by(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& add_group_by(std::string_view column) {
        query_.add_group_by(import_entity_column(column));
        return *this;
    }
    db_query_builder& having(const db_predicate& predicate) {
        query_.having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_having(const db_predicate& predicate) {
        query_.and_having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_having(const db_predicate& predicate) {
        query_.or_having(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& order_by(const db_expr& expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.order_by(expression.expression(query_, entity_type::table_name(), alias_), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(const db_expr& expression, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.add_order_by(expression.expression(query_, entity_type::table_name(), alias_), direction, nulls);
        return *this;
    }
    db_query_builder& order_by(const db_order_term& term) {
        return order_by(term.expression_, term.direction_, term.nulls_);
    }
    db_query_builder& order_by(std::span<const db_order_term> terms) {
        for (const auto& term : terms) {
            if (term.expression_.empty()) {
                throw std::invalid_argument("cannot evaluate an empty database expression");
            }
        }
        query_.clear_order();
        for (const auto& term : terms) {
            add_order_by(term);
        }
        return *this;
    }
    db_query_builder& order_by(std::initializer_list<db_order_term> terms) {
        return order_by(std::span<const db_order_term>(terms.begin(), terms.size()));
    }
    db_query_builder& add_order_by(const db_order_term& term) {
        return add_order_by(term.expression_, term.direction_, term.nulls_);
    }
    db_query_builder& add_order_by(std::span<const db_order_term> terms) {
        for (const auto& term : terms) {
            add_order_by(term);
        }
        return *this;
    }
    db_query_builder& add_order_by(std::initializer_list<db_order_term> terms) {
        return add_order_by(std::span<const db_order_term>(terms.begin(), terms.size()));
    }
    db_query_builder& distinct(bool enabled = true) {
        query_.distinct(enabled);
        return *this;
    }
    db_query_builder& distinct_on(std::span<const db_expr> expressions) {
        detail::import_db_expressions(query_, expressions, [this](const db_expr& expression) { return import_entity_expression(expression); }, [this](std::span<const db_expression> imported) { query_.distinct_on(imported); });
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<db_expr> expressions) {
        return distinct_on(std::span<const db_expr>(expressions.begin(), expressions.size()));
    }
    db_query_builder& distinct_on(std::span<const std::string_view> columns) {
        detail::import_db_expressions(query_, columns, [this](std::string_view column) { return import_entity_column(column); }, [this](std::span<const db_expression> imported) { query_.distinct_on(imported); });
        return *this;
    }
    db_query_builder& distinct_on(std::initializer_list<std::string_view> columns) {
        return distinct_on(std::span<const std::string_view>(columns.begin(), columns.size()));
    }
    db_query_builder& where(const db_predicate& predicate) {
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& and_where(const db_predicate& predicate) {
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& or_where(const db_predicate& predicate) {
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_query_builder& clear_where() {
        query_.clear_where();
        return *this;
    }
    db_query_builder& order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.order_by(import_entity_column(column), direction, nulls);
        return *this;
    }
    db_query_builder& add_order_by(std::string_view column, db_order_direction direction = db_order_direction::asc, db_nulls_order nulls = db_nulls_order::default_value) {
        query_.add_order_by(import_entity_column(column), direction, nulls);
        return *this;
    }
    db_query_builder& clear_order() {
        query_.clear_order();
        return *this;
    }
    db_query_builder& offset(std::optional<query_row_count> count) {
        query_.offset(count);
        return *this;
    }
    db_query_builder& limit(std::optional<query_row_count> count) {
        query_.limit(count);
        return *this;
    }
    db_query_builder& with_deleted(bool enabled = true) noexcept {
        with_deleted_ = enabled;
        return *this;
    }
    db_query_builder& left_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::left, condition.expression(query_, entity_type::table_name(), alias_));
    }
    db_query_builder& inner_join_and_select(std::string_view relation, std::string_view alias, const db_predicate& condition = {}) {
        return join_and_select(relation, alias, db_join_type::inner, condition.expression(query_, entity_type::table_name(), alias_));
    }
    db_query_builder& cache(const db_cache_setting_type& setting) {
        query_.cache(setting);
        return *this;
    }
    db_query_builder& cache(std::string_view id, std::optional<std::chrono::milliseconds> duration = {}) {
        query_.cache(id, duration);
        return *this;
    }
    db_query_builder& set_lock(const db_lock_options& options) {
        query_.lock(options);
        return *this;
    }
    db_query_builder& clear_lock() {
        query_.clear_lock();
        return *this;
    }

    template <typename output_type = entity_type>
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        auto mapper = projection_mapper<output_type>();
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(target_query);
                return executor_.template query_mapped<entity_rows<entity_type>>(prepared ? *prepared : target_query,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<entity_rows<output_type>>(target_query, std::move(mapper));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        auto mapper = projection_mapper<output_type>();
        auto query = single_row_query();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(query);
                return executor_.template query_mapped<std::optional<entity_type>>(prepared ? *prepared : query,
                    detail::db_map_one_related_entity<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped<std::optional<output_type>>(query, detail::db_map_one_projection<output_type>{std::move(mapper)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<db_rows>(target_query, detail::db_map_raw_rows{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, db_rows>> get_raw_and_entities() const {
        auto mapper = projection_mapper<output_type>(true);
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<std::pair<entity_rows<output_type>, db_rows>>(
            target_query, detail::db_map_raw_and_entities<output_type>{std::move(mapper)});
    }
    // Scalars own all decoded data; get_scalar returns nullopt for no row or
    // SQL NULL, while get_scalars rejects NULL rows. Arrays remain one-dimensional.
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        auto query = single_row_query();
        return executor_.template query_mapped<std::optional<scalar_type>>(query, detail::db_map_scalar<scalar_type>{});
    }
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::vector<scalar_type>> get_scalars() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        return executor_.template query_mapped<std::vector<scalar_type>>(target_query, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] task<db_stream_result> stream() const
        requires std::same_as<executor_type, db_session>
    {
        executor_.registration_.require_active();
        auto statement = get_query_and_parameters();
        auto& scope = executor_.registration_.scope();
        return make_scoped_operation(scope,
            db_session::query_stream_prepared(executor_.client_, std::move(statement.sql_),
                std::move(statement.params_), executor_.resource_, scope, executor_.io_metrics_, executor_.options_));
    }
    [[nodiscard]] task<std::uint64_t> get_count() const {
        const auto count = count_query();
        return executor_.template query_mapped<std::uint64_t>(count, detail::db_map_count{});
    }
    [[nodiscard]] task<bool> get_exists() const {
        auto source_value = unpaged_source();
        db_query query;
        query.select(query.alias(query.exists(std::move(source_value)), "exists"));
        query.copy_cache(query_, detail::db_cache_shape::exists);
        return executor_.template query_mapped<bool>(query, detail::db_map_exists{});
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::pair<entity_rows<output_type>, std::uint64_t>> get_many_and_count() const {
        if (query_.has_writes()) {
            throw std::invalid_argument("get_many_and_count cannot execute a write CTE twice");
        }
        auto mapper = projection_mapper<output_type>();
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        auto count = count_query();
        if constexpr (std::same_as<output_type, entity_type>) {
            if (hydrates()) {
                auto prepared = prepare_relation_query(target_query);
                return executor_.template query_mapped_and_count<entity_rows<entity_type>>(prepared ? *prepared : target_query, count,
                    detail::db_map_related_entities<entity_type>{relations_->clone()});
            }
        }
        return executor_.template query_mapped_and_count<entity_rows<output_type>>(target_query, count, std::move(mapper));
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        std::optional<db_query> active_query;
        const auto& target_query = target(active_query);
        auto prepared = prepare_relation_query(target_query);
        return (prepared ? *prepared : target_query).compile(executor_.query_driver());
    }

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    void require_plain_projection() const {
        if (hydrates()) {
            throw std::invalid_argument("explicit projections and subqueries require joins without relation hydration");
        }
    }
    [[nodiscard]] db_expression import_entity_expression(const db_expr& expression) {
        return expression.expression(query_, entity_type::table_name(), alias_);
    }
    [[nodiscard]] db_expression import_entity_column(std::string_view column) {
        detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        return query_.column(column, alias_);
    }
    template <typename output_type>
    detail::db_map_projection<output_type> projection_mapper(bool allow_extra_columns = false) const {
        if constexpr (!std::same_as<output_type, entity_type>) {
            require_plain_projection();
            if (selected_.empty()) {
                throw std::invalid_argument("DTO mapping requires an explicit projection");
            }
        }
        return detail::db_map_projection<output_type>(selected_.columns_, allow_extra_columns, selected_.wildcard_);
    }
    db_query_builder(detail::db_repository_input_type<executor_type> executor, std::string_view alias)
        : executor_(executor),
          alias_(alias.empty() ? detail::unqualified_db_name(entity_type::table_name()) : alias) {
        detail::select_entity<entity_type>(query_, alias_);
    }
    [[nodiscard]] bool hydrates() const noexcept {
        return relations_ && !relations_->empty();
    }
    [[nodiscard]] const db_query& target(std::optional<db_query>& scratch) const {
        scratch = effective_query();
        return scratch ? *scratch : query_;
    }
    [[nodiscard]] db_query single_row_query() const {
        std::optional<db_query> scratch;
        auto query = target(scratch).clone();
        query.limit(1);
        // An id-keyed cache includes the derived result shape.
        query.copy_cache(query_, detail::db_cache_shape::one);
        return query;
    }
    [[nodiscard]] db_query unpaged_source() const {
        std::optional<db_query> scratch;
        auto query = target(scratch).clone();
        query.clear_order().clear_lock().limit(std::nullopt).offset(std::nullopt);
        return query;
    }
    [[nodiscard]] std::optional<db_query> effective_query() const {
        constexpr auto del_col = detail::delete_date_column<entity_type>();
        if constexpr (!del_col.empty()) {
            if (!with_deleted_) {
                auto filtered = query_.clone();
                const auto qualifier = alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
                filtered.and_where(filtered.unary(db_unary_operator::is_null, filtered.column(del_col, qualifier)));
                return filtered;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<db_query> prepare_relation_query(const db_query& target_query) const {
        if (hydrates()) {
            return relations_->template prepare<entity_type>(target_query, alias_, executor_.query_driver());
        }
        return std::nullopt;
    }
    [[nodiscard]] db_query count_query() const {
        auto source_value = unpaged_source();
        if (hydrates()) {
            db_query roots;
            constexpr auto keys = detail::db_primary_key_columns<entity_type>();
            for (const auto key : keys) {
                roots.add_select(roots.column(key, "__ruvia_count_roots"));
            }
            roots.from(std::move(source_value), "__ruvia_count_roots").distinct();
            source_value = std::move(roots);
        }
        db_query count;
        const std::array args{count.star()};
        count.select(count.alias(count.aggregate("count", args), "count")).from(std::move(source_value), "count_source");
        count.copy_cache(query_, detail::db_cache_shape::count);
        return count;
    }
    db_query_builder& join_and_select(std::string_view relation, std::string_view alias, db_join_type join, db_expression condition = {}) {
        if (query_.has_writes()) {
            throw std::invalid_argument("write CTEs require flat result mapping");
        }
        if (!selected_.empty()) {
            throw std::invalid_argument("relation hydration requires a full entity projection");
        }
        if (alias.empty()) {
            throw std::invalid_argument("relation joins require an alias");
        }
        if (!relations_) {
            relations_.emplace();
        }
        relations_->template add<entity_type>(query_, alias_, relation, alias, join, condition);
        return *this;
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    std::string alias_;
    detail::db_projection selected_;
    std::optional<detail::db_relation_plan> relations_{};
    bool with_deleted_{false};
};

template <typename entity_type, typename executor_type>
class db_write_query_builder final {
public:
    db_write_query_builder(const db_write_query_builder&) = delete;
    db_write_query_builder& operator=(const db_write_query_builder&) = delete;
    db_write_query_builder(db_write_query_builder&&) = default;
    db_write_query_builder& operator=(db_write_query_builder&&) = delete;

    db_write_query_builder& where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& and_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.and_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& or_where(const db_predicate& predicate) {
        require_condition(predicate);
        query_.or_where(predicate.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    db_write_query_builder& set(std::string_view column, const db_expr& value) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_ || value.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, value.expression(query_, entity_type::table_name(), alias_));
        return *this;
    }
    template <detail::db_parameter v_type>
        requires(!std::same_as<std::remove_cvref_t<v_type>, db_expression> &&
                 !std::same_as<std::remove_cvref_t<v_type>, db_expr>)
    db_write_query_builder& set(std::string_view column, v_type&& value) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        if constexpr (std::same_as<std::remove_cvref_t<v_type>, std::nullptr_t>) {
            return set_null(column);
        } else {
            query_.set(column, query_.value(detail::make_immediate_db_parameter(std::forward<v_type>(value))));
            return *this;
        }
    }
    db_write_query_builder& set_null(std::string_view column) {
        const auto& info = detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        if (!info.writable_ || !info.nullable_) {
            throw std::invalid_argument("SET NULL requires a writable nullable entity column");
        }
        query_.set(column, query_.null_value());
        return *this;
    }
    db_write_query_builder& set_now(std::string_view column) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, column).writable_) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        query_.set(column, query_.call("now"));
        return *this;
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& increment(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::add);
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    db_write_query_builder& decrement(std::string_view column, number_type value = number_type{1}) {
        return adjust_number(column, value, db_binary_operator::subtract);
    }
    db_write_query_builder& set(const db_assignment& assignment) {
        query_.set(prepare_assignment(assignment));
        return *this;
    }
    db_write_query_builder& set(std::span<const db_assignment> assignments) {
        if (assignments.empty()) {
            throw std::invalid_argument("SET requires at least one writable column");
        }
        std::array<db_assignment, column_capacity> inline_staged{};
        std::vector<db_assignment> heap_staged;
        std::span<db_assignment> staged;
        if (assignments.size() <= inline_staged.size()) {
            staged = std::span(inline_staged).first(assignments.size());
        } else {
            heap_staged.resize(assignments.size());
            staged = heap_staged;
        }
        for (std::size_t index = 0; index < assignments.size(); ++index) {
            staged[index] = prepare_assignment(assignments[index]);
        }
        query_.set(staged);
        return *this;
    }
    db_write_query_builder& set(std::initializer_list<db_assignment> assignments) {
        return set(std::span<const db_assignment>(assignments.begin(), assignments.size()));
    }
    db_write_query_builder& set(const entity_type& changes) {
        std::array<db_assignment, column_capacity> staged{};
        std::size_t count = 0;
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (detail::is_writable_db_column<c_type>()) {
                if (changes.template is_set<c_type::name>()) {
                    staged[count++] = {c_type::name.view(), db_expr(detail::entity_column_value<entity_type, c_type>(query_, changes), {}, true)};
                }
            }
        });
        if (count == 0 && detail::update_date_column<entity_type>().empty() && detail::version_column<entity_type>().empty()) {
            throw std::invalid_argument("SET requires at least one writable column");
        }
        query_.set(std::span<const db_assignment>(staged.data(), count));
        return *this;
    }
    template <typename source>
    db_write_query_builder& update_from(std::string_view alias = {}) {
        static_assert(sql_entity<source>, "SQL update sources require a SQL entity");
        query_.update_from(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& update_from(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.update_from(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& update_from_cte(std::string_view name, std::string_view alias = {}) {
        query_.update_from(name, alias);
        return *this;
    }
    template <typename source>
    db_write_query_builder& delete_using(std::string_view alias = {}) {
        static_assert(sql_entity<source>, "SQL delete sources require a SQL entity");
        query_.delete_using(source::table_name(), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& delete_using(const db_query_builder<source, source_executor_type>& source_value, std::string_view alias) {
        source_value.require_plain_projection();
        query_.delete_using(source_value.query_, alias);
        return *this;
    }
    db_write_query_builder& delete_using_cte(std::string_view name, std::string_view alias = {}) {
        query_.delete_using(name, alias);
        return *this;
    }
    template <typename joined_type>
    db_write_query_builder& join(db_join_type type, std::string_view alias, const db_predicate& on = {}) {
        static_assert(sql_entity<joined_type>, "SQL joins require a SQL entity");
        query_.join(type, joined_type::table_name(), detail::import_join_predicate<joined_type, entity_type>(query_, on, alias_, alias), alias);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& join(db_join_type type, const db_query_builder<source, source_executor_type>& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        source_value.require_plain_projection();
        query_.join(type, source_value.query_, detail::import_join_predicate<source, entity_type>(query_, on, alias_, alias), alias, options);
        return *this;
    }
    db_write_query_builder& join(db_join_type type, const db_query& source_value,
        std::string_view alias, const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join(type, source_value, on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& join_cte(db_join_type type, std::string_view name, std::string_view alias, const db_predicate& on = {}) {
        query_.join(type, name, on.expression(query_, entity_type::table_name(), alias_), alias);
        return *this;
    }
    db_write_query_builder& join_function(db_join_type type, db_expression function, std::string_view alias,
        const db_predicate& on = {}, const db_source_options& options = {}) {
        query_.join_function(type, query_.import_expression(function), on.expression(query_, entity_type::table_name(), alias_), alias, options);
        return *this;
    }
    db_write_query_builder& values(const entity_type& entity) {
        return values(std::span<const entity_type>(&entity, 1));
    }
    db_write_query_builder& values(std::initializer_list<entity_type> entities) {
        return values(std::span<const entity_type>(entities.begin(), entities.size()));
    }
    db_write_query_builder& values(std::span<const entity_type> entities) {
        if (!inserting_) {
            throw std::invalid_argument("values requires an insert builder");
        }
        std::array<std::string_view, column_capacity> columns{};
        std::size_t column_count = 0;
        detail::append_entity_insert_rows(query_, entities, columns, column_count);
        record_inserted_columns(std::span<const std::string_view>(columns.data(), column_count));
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_plain_projection();
        source_value.query_.require_select_query();
        detail::validate_insert_columns(detail::db_entity_columns<entity_type>, columns);
        const auto count = source_value.selected_.empty() ? std::tuple_size_v<typename source::columns_type> : source_value.selected_.columns_.size();
        if (!source_value.selected_.wildcard_ && count != columns.size()) {
            throw std::invalid_argument("insert_from projection and target column counts differ");
        }
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value.query_);
        inserted_column_count_ = 0;
        record_inserted_columns(columns);
        return *this;
    }
    template <typename source, typename source_executor_type>
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query_builder<source, source_executor_type>& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& insert_from(std::span<const std::string_view> columns, const db_query& source_value) {
        if (!inserting_) {
            throw std::invalid_argument("insert_from requires an insert builder");
        }
        source_value.require_select_query();
        detail::validate_insert_columns(detail::db_entity_columns<entity_type>, columns);
        query_.insert_into(entity_type::table_name(), columns, alias_).insert_from(source_value);
        inserted_column_count_ = 0;
        record_inserted_columns(columns);
        return *this;
    }
    db_write_query_builder& insert_from(std::initializer_list<std::string_view> columns, const db_query& source_value) {
        return insert_from(std::span<const std::string_view>(columns.begin(), columns.size()), source_value);
    }
    db_write_query_builder& on_conflict(const db_upsert_options& options) {
        if (!inserting_) {
            throw std::invalid_argument("on_conflict requires an insert builder");
        }
        detail::apply_entity_upsert<entity_type>(
            query_, std::span<const std::string_view>(inserted_columns_.data(), inserted_column_count_),
            options, executor_.query_driver(), qualifier());
        return *this;
    }
    template <typename source_type>
        requires requires(const source_type& source) {
            { source.query() } -> std::same_as<const db_query&>;
        }
    db_write_query_builder& with(std::string_view name, const source_type& source, const db_cte_options& options = {}) {
        return with(name, source.query(), options);
    }
    db_write_query_builder& with(std::string_view name, const db_query& source_value, const db_cte_options& options = {}) {
        query_.with(name, source_value, options);
        return *this;
    }
    db_write_query_builder& with(std::string_view name, const db_values_builder& values,
        db_materialization materialization = db_materialization::default_value) {
        query_.with_columns(name, values.query(), materialization, values.columns());
        return *this;
    }
    db_write_query_builder& returning(std::span<const db_selection> fields = {}) {
        if (fields.empty()) {
            std::array<db_expression, column_capacity> expressions{};
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t index> {
                expressions[index] = query_.column(c_type::name.view(), qualifier());
            });
            query_.returning(expressions);
            selected_.columns_.clear();
            selected_.wildcard_ = false;
        } else {
            for (const auto& field : fields) {
                if (!field.has_expression() && !field.is_wildcard()) {
                    detail::require_db_column(detail::db_entity_columns<entity_type>, field.column_);
                }
            }
            selected_ = detail::apply_projection(query_, fields, qualifier(), true, entity_type::table_name());
        }
        return *this;
    }
    db_write_query_builder& returning(std::initializer_list<db_selection> fields_value) {
        return returning(std::span<const db_selection>(fields_value.begin(), fields_value.size()));
    }
    db_write_query_builder& returning(std::span<const std::string_view> columns) {
        return detail::stage_db_selections<column_capacity>(columns,
            [this](std::span<const db_selection> selections) -> db_write_query_builder& { return returning(selections); });
    }
    [[nodiscard]] task<db_exec_result> execute() const {
        validate();
        return executor_.execute_query(query_);
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<entity_rows<output_type>> get_many() const {
        require_returning<output_type>("get_many requires RETURNING");
        return executor_.template query_mapped<entity_rows<output_type>>(query_, detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_));
    }
    template <typename output_type = entity_type>
    [[nodiscard]] task<std::optional<output_type>> get_one() const {
        require_returning<output_type>("get_one requires RETURNING");
        return executor_.template query_mapped<std::optional<output_type>>(query_,
            detail::db_map_one_projection<output_type>{detail::db_map_projection<output_type>(selected_.columns_, false, selected_.wildcard_)});
    }
    [[nodiscard]] task<db_rows> get_raw_many() const {
        require_returning("get_raw_many requires RETURNING");
        return executor_.template query_mapped<db_rows>(query_, detail::db_map_raw_rows{});
    }
    // RETURNING uses the same owning value and NULL contract as read scalars.
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::optional<scalar_type>> get_scalar() const {
        require_returning("get_scalar requires RETURNING");
        return executor_.template query_mapped<std::optional<scalar_type>>(query_, detail::db_map_scalar<scalar_type>{});
    }
    template <db_mapped_value scalar_type>
    [[nodiscard]] task<std::vector<scalar_type>> get_scalars() const {
        require_returning("get_scalars requires RETURNING");
        return executor_.template query_mapped<std::vector<scalar_type>>(query_, detail::db_map_scalars<scalar_type>{});
    }
    [[nodiscard]] db_statement get_query_and_parameters() const {
        validate();
        return query_.compile(executor_.query_driver());
    }
    // Borrows the complete validated DML AST, including RETURNING and parameters.
    // The builder and its resource must outlive the view; with() clones on import.
    [[nodiscard]] const db_query& query() const& RUVIA_LIFETIMEBOUND {
        validate();
        return query_;
    }
    const db_query& query() const&& = delete;

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_repository<entity_type, executor_type>;
    template <typename, typename>
    friend class db_query_builder;
    template <typename, typename>
    friend class db_write_query_builder;
    db_write_query_builder(detail::db_repository_input_type<executor_type> executor, db_query query, bool inserting,
        std::string_view alias = {}, std::span<const std::string_view> inserted_columns = {})
        : executor_(executor),
          query_(std::move(query)),
          inserting_(inserting),
          alias_(alias) {
        record_inserted_columns(inserted_columns);
    }
    std::string_view qualifier() const {
        return alias_.empty() ? entity_type::table_name() : std::string_view(alias_);
    }
    db_assignment prepare_assignment(const db_assignment& assignment) {
        if (!detail::require_db_column(detail::db_entity_columns<entity_type>, assignment.column_).writable_ || assignment.empty()) {
            throw std::invalid_argument("SET requires a writable entity column and expression");
        }
        return {assignment.column_, db_expr(assignment.expression(query_, entity_type::table_name(), alias_), {}, true)};
    }
    template <typename number_type>
    db_write_query_builder& adjust_number(std::string_view column, number_type value, db_binary_operator op) {
        const auto& info = detail::require_db_column(detail::db_entity_columns<entity_type>, column);
        if (!info.writable_ || !info.numeric_) {
            throw std::invalid_argument("increment/decrement requires a writable numeric column");
        }
        query_.set(column, query_.binary(query_.column(column, qualifier()), op, query_.value(value)));
        return *this;
    }
    void record_inserted_columns(std::span<const std::string_view> columns) noexcept {
        for (const auto& column : detail::db_entity_columns<entity_type>) {
            if (column.writable_) {
                const auto end = inserted_columns_.begin() + inserted_column_count_;
                if (std::ranges::find(columns, column.name_) != columns.end() &&
                    std::find(inserted_columns_.begin(), end, column.name_) == end) {
                    inserted_columns_[inserted_column_count_++] = column.name_;
                }
            }
        }
    }
    void require_condition(const db_predicate& condition) const {
        if (inserting_ || condition.empty()) {
            throw std::invalid_argument("write WHERE requires an update/delete builder and a nonempty condition");
        }
    }
    void validate() const {
        if (!inserting_ && !query_.has_where()) {
            throw std::invalid_argument("repository update/delete requires a condition");
        }
    }
    template <typename output_type = entity_type>
    void require_returning(const char* message) const {
        validate();
        if (!query_.returns_rows()) {
            throw std::invalid_argument(message);
        }
        if constexpr (!std::same_as<output_type, entity_type>) {
            if (selected_.empty()) {
                throw std::invalid_argument("DTO returning requires an explicit projection");
            }
        }
    }
    detail::db_repository_executor_type<executor_type> executor_;
    db_query query_;
    bool inserting_;
    std::string alias_;
    detail::db_projection selected_;
    std::array<std::string_view, column_capacity> inserted_columns_{};
    std::size_t inserted_column_count_{0};
};

template <typename entity_type, typename executor_type>
class db_repository final {
    static_assert(sql_entity<entity_type>, "SQL repositories require a RUVIA_DB_ENTITY declaration");

public:
    [[nodiscard]] db_query_builder<entity_type, executor_type> query_builder(std::string_view alias = {}) const {
        return db_query_builder<entity_type, executor_type>(executor_, alias);
    }
    // Update builders start with the lifecycle stamps; set() on update_date_
    // or version_ replaces the stamp.
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> update_builder(std::string_view alias = {}) const {
        db_query query;
        query.update(entity_type::table_name(), alias);
        detail::stamp_lifecycle_columns<entity_type>(query, alias.empty() ? entity_type::table_name() : alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> delete_builder(std::string_view alias = {}) const {
        db_query query;
        query.delete_from(entity_type::table_name(), alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), false, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::string_view alias = {}) const {
        db_query query;
        query.insert_into(entity_type::table_name(), {}, alias);
        return db_write_query_builder<entity_type, executor_type>(executor_, std::move(query), true, alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(const entity_type& entity, std::string_view alias = {}) const {
        return insert_builder(std::span<const entity_type>(&entity, 1), alias);
    }
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> insert_builder(std::span<const entity_type> entities, std::string_view alias = {}) const {
        auto inserted = insert_query(entities, alias);
        return db_write_query_builder<entity_type, executor_type>(
            executor_, std::move(inserted.query_), true, alias, inserted.columns());
    }
    [[nodiscard]] task<entity_rows<entity_type>> find(const db_find_options& options = {}) const {
        return find_builder(options).get_many();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<entity_rows<entity_type>> find(const predicate_type& where) const {
        return find(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::optional<entity_type>> find_one(const db_find_options& options = {}) const {
        return find_builder(options).get_one();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<std::optional<entity_type>> find_one(const predicate_type& where) const {
        return find_one(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<std::pair<entity_rows<entity_type>, std::uint64_t>> find_and_count(const db_find_options& options = {}) const {
        return find_builder(options).get_many_and_count();
    }
    [[nodiscard]] task<std::uint64_t> count(const db_find_options& options = {}) const {
        return find_builder(options).get_count();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<std::uint64_t> count(const predicate_type& where) const {
        return count(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<bool> exists(const db_find_options& options = {}) const {
        return find_builder(options).get_exists();
    }
    template <std::same_as<db_predicate> predicate_type>
    [[nodiscard]] task<bool> exists(const predicate_type& where) const {
        return exists(db_find_options{.where_ = where});
    }
    [[nodiscard]] task<entity_type> save(const entity_type& entity) const {
        auto builder = save_builder(std::span<const entity_type>(&entity, 1));
        builder.returning();
        return executor_.template query_mapped<entity_type>(
            builder.query(), detail::db_map_required_one<entity_type>{detail::db_map_projection<entity_type>({})});
    }
    [[nodiscard]] task<entity_rows<entity_type>> save(std::span<const entity_type> entities) const {
        return save_builder(entities).returning().get_many();
    }
    [[nodiscard]] task<db_exec_result> soft_delete(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository soft_delete requires a condition");
        }
        auto builder = update_builder();
        builder.set_now(detail::delete_date_column<entity_type>());
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> restore(const db_predicate& predicate) const
        requires(!detail::delete_date_column<entity_type>().empty())
    {
        if (predicate.empty()) {
            throw std::invalid_argument("repository restore requires a condition");
        }
        auto builder = update_builder();
        builder.set_null(detail::delete_date_column<entity_type>());
        builder.where(predicate);
        return builder.execute();
    }
    [[nodiscard]] task<db_exec_result> clear() const {
        auto sql = detail::format_truncate_table(entity_type::table_name(), executor_.query_driver());
        return executor_.execute_prepared(std::move(sql));
    }
    [[nodiscard]] task<db_exec_result> insert(const entity_type& entity) const {
        return insert(std::span<const entity_type>(&entity, 1));
    }
    [[nodiscard]] task<db_exec_result> insert(std::span<const entity_type> entities) const {
        return insert_builder(entities).execute();
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, const entity_type& changes) const {
        return update_builder().set(changes).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> delete_by(const db_predicate& predicate) const {
        return delete_builder().where(predicate).execute();
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> increment(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return update_builder().increment(property_path, value).where(predicate).execute();
    }
    template <typename number_type = std::int64_t>
        requires((std::integral<number_type> || std::floating_point<number_type>) && !std::same_as<number_type, bool>)
    [[nodiscard]] task<db_exec_result> decrement(const db_predicate& predicate, std::string_view property_path, number_type value = number_type{1}) const {
        return update_builder().decrement(property_path, value).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> remove(const entity_type& entity) const {
        db_query query;
        query.delete_from(entity_type::table_name());
        detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
            if constexpr (c_type::options.primary_key_) {
                if (!entity.template is_set<c_type::name>() || entity.template is_null<c_type::name>()) {
                    throw std::invalid_argument("remove requires all primary key values");
                }
                query.and_where(query.binary(query.column(c_type::name.view()), db_binary_operator::equal, detail::entity_column_value<entity_type, c_type>(query, entity)));
            }
        });
        if (!query.has_where()) {
            throw std::invalid_argument("remove requires an entity primary key");
        }
        return executor_.execute_query(query);
    }
    [[nodiscard]] task<db_exec_result> upsert(const entity_type& entity, const db_upsert_options& options) const {
        return upsert(std::span<const entity_type>(&entity, 1), options);
    }
    [[nodiscard]] task<db_exec_result> upsert(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto query = upsert_query(entities, options);
        return executor_.execute_query(query);
    }

    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::span<const db_assignment> changes) const {
        return update_builder().set(changes).where(predicate).execute();
    }
    [[nodiscard]] task<db_exec_result> update(const db_predicate& predicate, std::initializer_list<db_assignment> changes) const {
        return update(predicate, std::span<const db_assignment>(changes.begin(), changes.size()));
    }

private:
    static constexpr std::size_t column_capacity = std::tuple_size_v<typename entity_type::columns_type>;
    friend class db_session;
    friend class db_transaction;
    explicit db_repository(detail::db_repository_input_type<executor_type> executor)
        : executor_(executor) {}
    [[nodiscard]] db_write_query_builder<entity_type, executor_type> save_builder(std::span<const entity_type> entities) const {
        constexpr auto keys = detail::db_primary_key_columns<entity_type>();
        if constexpr (keys.empty()) {
            return insert_builder(entities);
        } else {
            bool all_keys_set = true;
            detail::for_each_descriptor<typename entity_type::columns_type>([&]<typename c_type, std::size_t> {
                if constexpr (c_type::options.primary_key_) {
                    for (const auto& entity : entities) {
                        all_keys_set &= entity.template is_set<c_type::name>() && !entity.template is_null<c_type::name>();
                    }
                }
            });
            if (!all_keys_set) {
                return insert_builder(entities);
            }
            auto builder = insert_builder(entities);
            const auto driver = executor_.query_driver();
            std::array<borrowed_text, keys.size()> conflict_columns{};
            std::size_t conflict_count = 0;
            if (driver == db_driver::postgresql) {
                for (const auto key : keys) {
                    conflict_columns[conflict_count++] = key;
                }
            }
            detail::require_uniform_set_columns<entity_type>(entities, {});
            builder.on_conflict(db_upsert_options{
                .conflict_paths_ = std::span<const borrowed_text>(conflict_columns.data(), conflict_count),
                .any_unique_key_ = driver != db_driver::postgresql,
            });
            return builder;
        }
    }
    [[nodiscard]] db_query_builder<entity_type, executor_type> find_builder(const db_find_options& options) const {
        auto builder = query_builder();
        builder.with_deleted(options.with_deleted_);
        if (!options.select_.empty()) {
            detail::stage_db_selections<column_capacity>(options.select_.view(),
                [&builder](std::span<const db_selection> selections) -> auto& { return builder.select(selections); });
        }
        detail::apply_find_options<entity_type>(builder.query_, options, builder.alias_);
        if (!options.relations_.empty()) {
            builder.relations_.emplace();
            for (const auto& path : options.relations_) {
                builder.relations_->template add<entity_type>(builder.query_, builder.alias_, path);
            }
        }
        return builder;
    }
    db_query upsert_query(std::span<const entity_type> entities, const db_upsert_options& options) const {
        auto inserted = insert_query(entities);
        if (!options.do_nothing_ && options.update_columns_.empty()) {
            detail::require_uniform_set_columns<entity_type>(entities, options.update_expressions_.view());
        }
        detail::apply_entity_upsert<entity_type>(inserted.query_, inserted.columns(), options, executor_.query_driver(), entity_type::table_name());
        return std::move(inserted.query_);
    }
    struct insert_query_result final {
        db_query query_;
        std::array<std::string_view, column_capacity> columns_{};
        std::size_t count_{0};
        [[nodiscard]] std::span<const std::string_view> columns() const noexcept {
            return std::span<const std::string_view>(columns_.data(), count_);
        }
    };
    insert_query_result insert_query(std::span<const entity_type> entities, std::string_view alias = {}) const {
        insert_query_result result{db_query{}};
        result.query_.insert_into(entity_type::table_name(), {}, alias);
        detail::append_entity_insert_rows(result.query_, entities, result.columns_, result.count_);
        return result;
    }
    detail::db_repository_executor_type<executor_type> executor_;
};

template <typename entity_type>
db_repository<entity_type, db_session> db_session::repository() const {
    registration_.require_active();
    return db_repository<entity_type, db_session>(*this);
}
template <typename entity_type>
db_repository<entity_type, db_transaction> db_transaction::repository() & {
    return db_repository<entity_type, db_transaction>(*this);
}

}  // namespace ruvia
