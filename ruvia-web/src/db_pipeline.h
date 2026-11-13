#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/core/worker_wait.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_session.h"

#include "client_tcp_transport.h"
#include "connection_lifecycle.h"
#include "operation_interrupt.h"

#ifdef _WIN32
#include <winsock2.h>

#include <asio/ip/tcp.hpp>
#else
#include <asio/posix/stream_descriptor.hpp>
#endif

namespace ruvia::detail {

// SHA-256 over input fed in pieces, the one digest the database layer keys
// statements, cache entries and migration history by. Input that fits the
// inline buffer is digested in one call; longer input streams through an
// OpenSSL context created once it overflows.
class db_sha256 final {
public:
    static constexpr std::size_t digest_size = 32;
    using digest_type = std::array<unsigned char, digest_size>;

    db_sha256() noexcept = default;
    static_assert(digest_size == SHA256_DIGEST_LENGTH);

    ~db_sha256() {
        if (context_ != nullptr) {
            EVP_MD_CTX_free(context_);
        }
    }

    db_sha256(const db_sha256&) = delete;
    db_sha256& operator=(const db_sha256&) = delete;

    void update(std::string_view bytes) {
        if (bytes.empty()) {
            return;
        }
        if (context_ == nullptr) {
            if (bytes.size() <= buffer_.size() - buffered_) {
                std::memcpy(buffer_.data() + buffered_, bytes.data(), bytes.size());
                buffered_ += bytes.size();
                return;
            }
            context_ = EVP_MD_CTX_new();
            if (context_ == nullptr || EVP_DigestInit_ex(context_, EVP_sha256(), nullptr) != 1 ||
                (buffered_ != 0 && EVP_DigestUpdate(context_, buffer_.data(), buffered_) != 1)) {
                throw_digest_failure();
            }
        }
        if (EVP_DigestUpdate(context_, bytes.data(), bytes.size()) != 1) {
            throw_digest_failure();
        }
    }

    // Eight little-endian bytes.
    void update_u64_le(std::uint64_t value) {
        std::array<char, 8> encoded;
        for (unsigned index = 0; index < encoded.size(); ++index) {
            encoded[index] = static_cast<char>((value >> (index * 8)) & 255);
        }
        update({encoded.data(), encoded.size()});
    }

    // Ends the digest; nothing may be fed afterwards.
    [[nodiscard]] digest_type finish() {
        digest_type digest;
        if (context_ == nullptr) {
            if (SHA256(buffer_.data(), buffered_, digest.data()) == nullptr) {
                throw_digest_failure();
            }
            return digest;
        }
        unsigned int size = 0;
        if (EVP_DigestFinal_ex(context_, digest.data(), &size) != 1 || size != digest.size()) {
            throw_digest_failure();
        }
        return digest;
    }

private:
    [[noreturn]] static void throw_digest_failure() {
        throw std::runtime_error("database SHA-256 digest failed");
    }

    std::array<unsigned char, 512> buffer_;
    std::size_t buffered_{0};
    evp_md_ctx_st* context_{nullptr};
};

[[nodiscard]] inline db_sha256::digest_type db_sha256_of(std::string_view bytes) {
    db_sha256 digest;
    digest.update(bytes);
    return digest.finish();
}

}  // namespace ruvia::detail

// Execution of compiled db_query statements: one deadline and one chain for a
// query, or a query and its count, run directly or through the result cache.

namespace ruvia {
class db_query;

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.
struct prepared_db_statement final {
    std::string sql_;
    std::vector<db_value> params_;
};

// Raw SQL is checked once, where the caller hands it over: it must hold a
// statement, and no NUL byte, since both client libraries read SQL as a C
// string. Compiled SQL is built that way; parameters are checked where they
// are encoded for the client library.
void validate_db_sql_text(std::string_view sql);

[[nodiscard]] prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params);

// PostgreSQL NOTIFY as a pg_notify() call, so channel and payload travel as
// bound parameters rather than as SQL text.
[[nodiscard]] prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload);

namespace detail {

[[nodiscard]] inline db_value clone_db_value_for_resource(
    const db_value& value) {
    switch (value.type()) {
        case db_value_type::null:
            return db_value(nullptr);
        case db_value_type::string:
            return db_value_access::owned_string(
                std::string(value.text()));
        case db_value_type::signed_value:
            return db_value(value.signed_value());
        case db_value_type::unsigned_value:
            return db_value(value.unsigned_value());
        case db_value_type::double_value:
            return db_value(value.double_value());
        case db_value_type::bool_value:
            return db_value(value.bool_value());
    }
    return db_value(nullptr);
}

[[nodiscard]] inline std::vector<db_value> clone_db_values(
    std::span<const db_value> values) {
    std::vector<db_value> output;
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(clone_db_value_for_resource(value));
    }
    return output;
}

struct request_io_metrics;

#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

// Where a plan's statements run: a pooled connection, or the slot an open
// transaction holds. A value adapter, not a lease owner.
struct db_query_backend final {
    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    // The request's counter block; null outside a request.
    request_io_metrics* io_metrics_{nullptr};
    // Set when the database statement itself failed: a transaction statement
    // failure has already retired the transaction's lease.
    bool* backend_failed_{nullptr};
#ifdef RUVIA_ENABLE_REDIS
    // Pooled backends only: a transaction's reads may include its own
    // uncommitted writes, which the shared cache must never see or replace.
    db_query_cache_state* cache_{nullptr};
#endif
};

struct db_query_step final {
    std::string sql_;
    std::vector<db_value> params_;
#ifdef RUVIA_ENABLE_REDIS
    // Present only when a configured cache applies to this statement.
    std::optional<std::string> cache_key_;
    std::optional<std::chrono::milliseconds> cache_duration_;
#endif
};

// Preparation owns every statement and cache key before returning a cold task.
// Transaction state and its exclusive lease remain with the outer operation.
struct db_query_plan final {
    db_query_step first_;
    std::optional<db_query_step> second_;
    [[nodiscard]] static db_query_plan prepare(
        const db_query& query, const db_query* count, const db_query_backend& backend);
};

template <bool with_count>
using db_query_result = std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>;

template <bool with_count>
[[nodiscard]] task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options);

// Runs one statement on the backend without a cache. `options` is owned by the
// awaiting frame, which must outlive the returned task; a pooled statement
// takes it over, a transaction statement borrows it.
[[nodiscard]] task<db_rows> query_db_backend(const db_query_backend& backend, std::string sql,
    std::vector<db_value> params, operation_options& options);

[[nodiscard]] inline operation_options db_remaining_options(
    const operation_options& base, const operation_deadline& timeout) {
    auto result_value = base;
    result_value.timeout_ = timeout.timeout();
    return result_value;
}

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

inline void require_db_dialect(db_driver driver) {
    if (driver != db_driver::postgresql && driver != db_driver::mariadb) {
        throw std::invalid_argument("SQL compilation requires an explicit database driver");
    }
}

// MariaDB limits identifiers to 64 characters and PostgreSQL to 63 bytes
// (NAMEDATALEN - 1). Counting characters requires well-formed UTF-8, so an
// ill-formed MariaDB name is rejected rather than measured by guesswork.
[[nodiscard]] inline bool fits_db_identifier_limit(std::string_view name, db_driver driver) noexcept {
    if (driver == db_driver::postgresql) {
        return name.size() <= 63;
    }
    std::size_t characters = 0;
    for (std::size_t i = 0; i < name.size(); ++characters) {
        if (static_cast<unsigned char>(name[i]) < 0x80) {
            ++i;
            continue;
        }
        const auto sequence = ::ruvia::scan_utf8_sequence(name, i);
        if (!sequence.well_formed_) {
            return false;
        }
        i += sequence.length_;
    }
    return characters <= 64;
}

inline void validate_db_identifier(std::string_view name, db_driver driver) {
    require_db_dialect(driver);
    if (name.empty() || name.find('\0') != std::string_view::npos || !fits_db_identifier_limit(name, driver)) {
        throw std::invalid_argument("invalid or overlong database identifier");
    }
}

inline void append_db_identifier(std::string& output, std::string_view name, db_driver driver) {
    validate_db_identifier(name, driver);
    const char quote = driver == db_driver::postgresql ? '"' : '`';
    output.push_back(quote);
    for (auto at = name.find(quote); at != std::string_view::npos; at = name.find(quote)) {
        output.append(name.data(), at + 1);
        output.push_back(quote);
        name.remove_prefix(at + 1);
    }
    output.append(name);
    output.push_back(quote);
}

inline void append_db_qualified_identifier(std::string& output, std::string_view name, db_driver driver) {
    while (true) {
        const auto dot = name.find('.');
        append_db_identifier(output, name.substr(0, dot), driver);
        if (dot == std::string_view::npos) {
            return;
        }
        output.push_back('.');
        name.remove_prefix(dot + 1);
    }
}

inline void append_db_string_literal(std::string& output, std::string_view value, db_driver driver) {
    require_db_dialect(driver);
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database text cannot contain NUL");
    }
    if (driver == db_driver::mariadb) {
        // Hex text is independent of NO_BACKSLASH_ESCAPES and ANSI_QUOTES.
        // It also avoids allowing connection SQL modes to change DDL values.
        output.reserve(output.size() + 26U + value.size() * 2U);
        output += "CONVERT(X'";
        http_append_lower_hex(output, value);
        output += "' USING utf8mb4)";
        return;
    }
    output.reserve(output.size() + 3U + value.size());
    output += "E'";
    for (auto at = value.find_first_of("'\\"); at != std::string_view::npos;
        at = value.find_first_of("'\\")) {
        output.append(value.data(), at + 1);
        output.push_back(value[at]);
        value.remove_prefix(at + 1);
    }
    output.append(value);
    output.push_back('\'');
}

// Bound doubles travel as float8/DOUBLE, so a literal must keep that type: an
// unadorned 2 is an integer and 2.5 is exact NUMERIC/DECIMAL in both dialects,
// which changes division and overload resolution. PostgreSQL gets an explicit
// cast of a quoted constant (valid in DEFAULT, CHECK, generated columns and
// views, and immune to a preceding '-' forming a comment); MariaDB reads any
// literal with an exponent as DOUBLE.
inline void append_db_double_literal(std::string& output, double value, db_driver driver) {
    require_db_dialect(driver);
    if (driver == db_driver::postgresql) {
        output += "CAST('";
        append_db_number(output, value);
        output += "' AS DOUBLE PRECISION)";
        return;
    }
    const auto start = output.size();
    append_db_number(output, value);
    if (output.find_first_of("eE", start) == std::string::npos) {
        output += "e0";
    }
}

inline void append_db_literal(std::string& output, const db_value& value, db_driver driver) {
    require_db_dialect(driver);
    switch (value.type()) {
        case db_value_type::null:
            output += "NULL";
            return;
        case db_value_type::string:
            append_db_string_literal(output, value.text(), driver);
            return;
        case db_value_type::signed_value:
            append_db_number(output, value.signed_value());
            return;
        case db_value_type::unsigned_value:
            append_db_number(output, value.unsigned_value());
            return;
        case db_value_type::double_value:
            append_db_double_literal(output, value.double_value(), driver);
            return;
        case db_value_type::bool_value:
            output += value.bool_value() ? "TRUE" : "FALSE";
            return;
    }
    std::terminate();
}

enum class db_sql_type_usage : std::uint8_t { column,
    cast };

void append_db_expression(std::string& output, db_expression expression, db_driver driver);

template <typename contains_fn_type>
[[nodiscard]] inline std::string_view select_db_dollar_delimiter(
    std::array<char, 32>& delimiter_buf, contains_fn_type&& contains_delimiter) noexcept {
    std::string_view delimiter = "$ruvia$";
    for (std::uint64_t suffix = 1; contains_delimiter(delimiter); ++suffix) {
        std::string_view("$ruvia").copy(delimiter_buf.data(), 6);
        std::array<char, http_max_unsigned_decimal_size> digits;
        const auto formatted = http_format_unsigned(digits, suffix);
        std::size_t length = 6;
        formatted.copy(delimiter_buf.data() + length, formatted.size());
        length += formatted.size();
        delimiter_buf[length++] = '$';
        delimiter = std::string_view(delimiter_buf.data(), length);
    }
    return delimiter;
}

inline void append_db_dollar_body(std::string& output, std::string_view body) {
    std::array<char, 32> delimiter_buf;
    const auto delimiter = select_db_dollar_delimiter(
        delimiter_buf, [body](std::string_view candidate) noexcept {
            return body.find(candidate) != std::string_view::npos;
        });
    output.reserve(output.size() + delimiter.size() * 2U + body.size());
    output.append(delimiter);
    output.append(body);
    output.append(delimiter);
}

[[nodiscard]] inline std::string_view table_lock_mode_name(db_table_lock_mode mode) {
    switch (mode) {
        case db_table_lock_mode::access_share:
            return "ACCESS SHARE";
        case db_table_lock_mode::row_share:
            return "ROW SHARE";
        case db_table_lock_mode::row_exclusive:
            return "ROW EXCLUSIVE";
        case db_table_lock_mode::share_update_exclusive:
            return "SHARE UPDATE EXCLUSIVE";
        case db_table_lock_mode::share:
            return "SHARE";
        case db_table_lock_mode::share_row_exclusive:
            return "SHARE ROW EXCLUSIVE";
        case db_table_lock_mode::exclusive:
            return "EXCLUSIVE";
        case db_table_lock_mode::access_exclusive:
            return "ACCESS EXCLUSIVE";
    }
    throw std::invalid_argument("unknown table lock mode");
}

// PostgreSQL statements emitted both as migration steps and inside PL/pgSQL bodies.
inline void append_db_lock_table_statement(std::string& output, std::string_view table, db_table_lock_mode mode, bool nowait) {
    const auto mode_name = table_lock_mode_name(mode);
    output.append("LOCK TABLE ");
    append_db_qualified_identifier(output, table, db_driver::postgresql);
    output.append(" IN ");
    output.append(mode_name);
    output.append(" MODE");
    if (nowait) {
        output.append(" NOWAIT");
    }
}
inline void append_db_perform_statement(std::string& output, db_expression expression) {
    output.append("PERFORM ");
    append_db_expression(output, expression, db_driver::postgresql);
}

inline void append_db_type_name(std::string& output, db_data_type type,
    std::string_view custom_name, std::size_t length, unsigned precision, unsigned scale,
    bool array_value, db_driver driver, db_sql_type_usage usage = db_sql_type_usage::column) {
    require_db_dialect(driver);
    const bool pg = driver == db_driver::postgresql;
    if ((array_value || !custom_name.empty()) && !pg) {
        throw std::invalid_argument("array and named database types require PostgreSQL");
    }
    if (!custom_name.empty()) {
        if (type != db_data_type::inferred || length != 0 || precision != 0 || scale != 0) {
            throw std::invalid_argument("a named database type cannot also specify scalar type modifiers");
        }
        append_db_qualified_identifier(output, custom_name, driver);
        if (array_value) {
            output += "[]";
        }
        return;
    }
    if (length != 0 && type != db_data_type::char_value && type != db_data_type::varchar) {
        throw std::invalid_argument("length requires CHAR or VARCHAR");
    }
    if ((precision != 0 || scale != 0) && type != db_data_type::numeric) {
        throw std::invalid_argument("precision and scale require NUMERIC");
    }
    if (scale > precision || (precision > (pg ? 1000U : 65U))) {
        throw std::invalid_argument("invalid database numeric precision or scale");
    }
    const bool maria_cast = !pg && usage == db_sql_type_usage::cast;
    if (type == db_data_type::char_value) {
        if (length == 0) {
            throw std::invalid_argument("CHAR requires a positive length");
        }
        if (pg && length > 10485760) {
            throw std::invalid_argument("PostgreSQL CHAR length exceeds the supported maximum");
        }
        if (!pg && !maria_cast && length > 255) {
            throw std::invalid_argument("MariaDB CHAR length exceeds the supported maximum");
        }
    }
    switch (type) {
        case db_data_type::boolean:
            output += maria_cast ? "UNSIGNED" : "BOOLEAN";
            break;
        case db_data_type::small_int:
            output += maria_cast ? "SIGNED" : "SMALLINT";
            break;
        case db_data_type::integer:
            output += maria_cast ? "SIGNED" : "INTEGER";
            break;
        case db_data_type::big_int:
            output += maria_cast ? "SIGNED" : "BIGINT";
            break;
        case db_data_type::real:
            output += maria_cast ? "DOUBLE" : "REAL";
            break;
        case db_data_type::double_value:
            output += pg ? "DOUBLE PRECISION" : "DOUBLE";
            break;
        case db_data_type::text:
            output += maria_cast ? "CHAR" : "TEXT";
            break;
        case db_data_type::char_value:
            output += "CHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::varchar:
            if (length == 0) {
                throw std::invalid_argument("VARCHAR requires a positive length");
            }
            output += maria_cast ? "CHAR(" : "VARCHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::numeric:
            output += "DECIMAL";
            if (precision != 0) {
                output.push_back('(');
                append_db_number(output, static_cast<std::uint64_t>(precision));
                output.push_back(',');
                append_db_number(output, static_cast<std::uint64_t>(scale));
                output.push_back(')');
            } else if (!pg) {
                // MariaDB's bare DECIMAL is DECIMAL(10,0) and silently rounds.
                throw std::invalid_argument("MariaDB DECIMAL requires an explicit precision");
            }
            break;
        case db_data_type::date:
            output += "DATE";
            break;
        case db_data_type::timestamp:
            // Match PostgreSQL's microsecond precision; bare DATETIME truncates.
            output += pg ? "TIMESTAMP" : "DATETIME(6)";
            break;
        case db_data_type::json:
            if (maria_cast) {
                throw std::invalid_argument("MariaDB does not support CAST AS JSON");
            }
            output += "JSON";
            break;
        case db_data_type::jsonb:
        case db_data_type::uuid:
        case db_data_type::timestamp_tz:
        case db_data_type::interval:
        case db_data_type::inet:
        case db_data_type::cidr:
        case db_data_type::bytea:
            if (!pg) {
                throw std::invalid_argument("selected database type requires PostgreSQL");
            }
            switch (type) {
                case db_data_type::jsonb:
                    output += "JSONB";
                    break;
                case db_data_type::uuid:
                    output += "UUID";
                    break;
                case db_data_type::timestamp_tz:
                    output += "TIMESTAMPTZ";
                    break;
                case db_data_type::interval:
                    output += "INTERVAL";
                    break;
                case db_data_type::inet:
                    output += "INET";
                    break;
                case db_data_type::cidr:
                    output += "CIDR";
                    break;
                case db_data_type::bytea:
                    output += "BYTEA";
                    break;
                default:
                    std::terminate();
            }
            break;
        case db_data_type::array:
        case db_data_type::inferred:
            throw std::invalid_argument("SQL requires an explicit scalar type; use the array flag for arrays");
    }
    if (array_value) {
        output += "[]";
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

constexpr std::size_t no_db_node = std::numeric_limits<std::size_t>::max();

enum class db_node_kind : std::uint8_t {
    sql,
    column,
    star,
    value,
    default_value,
    excluded,
    function,
    coalesce,
    null_if,
    greatest,
    least,
    binary,
    unary,
    between,
    tuple,
    list,
    array,
    any,
    all,
    cast,
    alias,
    case_value,
    exists,
    subquery,
    aggregate,
    filter,
    window,
    within_group,
    extract,
    subscript,
    collate,
    at_time_zone,
};
enum class db_query_kind : std::uint8_t { select,
    values,
    insert,
    update,
    delete_value };
enum class db_source_kind : std::uint8_t { table,
    query,
    function };

struct db_stored_type final {
    db_stored_type() = default;
    db_stored_type(const db_stored_type&) = default;
    db_stored_type& operator=(const db_stored_type&) = default;
    db_stored_type(db_stored_type&&) noexcept = default;
    db_stored_type& operator=(db_stored_type&&) noexcept = default;
    db_stored_type(const db_type_definition& source_value)
        : data_type_(source_value.data_type_),
          custom_name_(source_value.custom_name_),
          length_(source_value.length_),
          precision_(source_value.precision_),
          scale_(source_value.scale_),
          array_(source_value.array_) {}
    db_data_type data_type_{db_data_type::inferred};
    std::string custom_name_;
    std::size_t length_{0};
    unsigned precision_{0};
    unsigned scale_{0};
    bool array_{false};
};
struct db_stored_order final {
    std::size_t expression_{no_db_node};
    db_order_direction direction_{db_order_direction::asc};
    db_nulls_order nulls_{db_nulls_order::default_value};
};
struct db_stored_named_argument final {
    db_stored_named_argument(std::string_view name, std::size_t expression)
        : name_(name),
          expression_(expression) {}
    std::string name_;
    std::size_t expression_;
};

struct db_query_node final {
    db_query_node(db_node_kind kind)
        : kind_(kind),
          value_(nullptr) {}
    db_query_node(db_node_kind kind, db_value&& value)
        : kind_(kind),
          value_(std::move(value)) {}
    db_query_node(const db_query_node& source_value)
        : kind_(source_value.kind_),
          value_(db_value(source_value.value_)),
          text_(source_value.text_),
          qualifier_(source_value.qualifier_),
          args_(source_value.args_),
          orders_(source_value.orders_),
          type_(source_value.type_),
          binary_(source_value.binary_),
          unary_(source_value.unary_),
          date_part_(source_value.date_part_),
          left_(source_value.left_),
          right_(source_value.right_),
          query_(source_value.query_),
          flag_(source_value.flag_),
          explicit_alias_(source_value.explicit_alias_),
          frame_(source_value.frame_) {
        named_.reserve(source_value.named_.size());
        for (const auto& argument : source_value.named_) {
            named_.emplace_back(argument.name_, argument.expression_);
        }
    }
    db_query_node(db_query_node&&) noexcept = default;
    db_query_node& operator=(db_query_node&&) = delete;
    db_node_kind kind_;
    db_value value_;
    std::string text_;
    std::string qualifier_;
    std::vector<std::size_t> args_;
    std::vector<db_stored_named_argument> named_;
    std::vector<db_stored_order> orders_;
    db_stored_type type_;
    db_binary_operator binary_{db_binary_operator::equal};
    db_unary_operator unary_{db_unary_operator::not_value};
    db_date_part date_part_{db_date_part::epoch};
    std::size_t left_{no_db_node};
    std::size_t right_{no_db_node};
    std::size_t query_{no_db_node};
    bool flag_{false};
    bool explicit_alias_{false};
    std::optional<db_window_frame_options> frame_{};
};
// AND and OR are associative, so a left-deep chain of one of them (as built
// by and_where/or_where or `p = std::move(p) && q`) is a single nesting level
// for import and compilation. True when `child` continues `parent`'s chain.
[[nodiscard]] inline bool continues_logical_chain(
    const std::vector<db_query_node>& nodes, const db_query_node& parent, std::size_t child) noexcept {
    if (parent.kind_ != db_node_kind::binary || child == no_db_node ||
        (parent.binary_ != db_binary_operator::and_value && parent.binary_ != db_binary_operator::or_value)) {
        return false;
    }
    const auto& node = nodes[child];
    return node.kind_ == db_node_kind::binary && node.binary_ == parent.binary_;
}
struct db_stored_source_column final {
    db_stored_source_column(const db_source_column& source_value)
        : name_(source_value.name_),
          type_(source_value.type_) {}
    db_stored_source_column(const db_stored_source_column&) = default;
    db_stored_source_column& operator=(const db_stored_source_column&) = default;
    db_stored_source_column(db_stored_source_column&&) noexcept = default;
    db_stored_source_column& operator=(db_stored_source_column&&) noexcept = default;
    std::string name_;
    db_stored_type type_;
};
struct db_query_source final {
    db_query_source() = default;
    db_query_source(const db_query_source&) = default;
    db_query_source& operator=(const db_query_source&) = default;
    db_query_source(db_query_source&&) noexcept = default;
    db_query_source& operator=(db_query_source&&) noexcept = default;
    db_source_kind kind_{db_source_kind::table};
    std::string name_;
    std::string alias_;
    std::size_t expression_{no_db_node};
    std::size_t query_{no_db_node};
    bool lateral_{false};
    bool ordinality_{false};
    std::vector<db_stored_source_column> columns_;
};
// Whether a FROM item binds `name`: its alias when present, otherwise its
// table name or, for a schema-qualified table, any dot-aligned suffix of it.
[[nodiscard]] inline bool source_binds_name(const db_query_source& source_value, std::string_view name) noexcept {
    if (!source_value.alias_.empty()) {
        return source_value.alias_ == name;
    }
    const std::string_view table = source_value.name_;
    return table == name ||
           (table.size() > name.size() && table.ends_with(name) && table[table.size() - name.size() - 1] == '.');
}
struct db_stored_join final {
    db_stored_join(db_join_type type, db_query_source source_value, std::size_t on)
        : type_(type),
          source_(std::move(source_value)),
          on_(on) {}
    db_stored_join(const db_stored_join& source_value)
        : type_(source_value.type_),
          source_(source_value.source_),
          on_(source_value.on_),
          using_columns_(source_value.using_columns_) {}
    db_join_type type_;
    db_query_source source_;
    std::size_t on_{no_db_node};
    std::vector<std::string> using_columns_;
};
struct db_stored_assignment final {
    db_stored_assignment(std::string_view column, std::size_t expression)
        : column_(column),
          expression_(expression) {}
    std::string column_;
    std::size_t expression_;
};
struct db_stored_cte final {
    template <class column_range>
    db_stored_cte(std::string_view name, std::size_t query, bool recursive, db_materialization materialization,
        const column_range& columns)
        : name_(name),
          query_(query),
          recursive_(recursive),
          materialization_(materialization) {
        columns_.reserve(std::ranges::size(columns));
        for (const auto& column : columns) {
            columns_.emplace_back(std::string_view(column));
        }
    }
    db_stored_cte(const db_stored_cte& source_value)
        : name_(source_value.name_),
          query_(source_value.query_),
          recursive_(source_value.recursive_),
          materialization_(source_value.materialization_),
          columns_(source_value.columns_) {}
    std::string name_;
    std::size_t query_;
    bool recursive_{false};
    db_materialization materialization_{db_materialization::default_value};
    std::vector<std::string> columns_;
};
struct db_stored_set_operation final {
    db_set_operation operation_;
    std::size_t query_;
};
struct db_stored_conflict final {
    db_stored_conflict() = default;
    db_stored_conflict(const db_stored_conflict&) = default;
    db_stored_conflict& operator=(const db_stored_conflict&) = default;
    db_stored_conflict(db_stored_conflict&&) noexcept = default;
    db_stored_conflict& operator=(db_stored_conflict&&) noexcept = default;
    std::vector<std::string> columns_;
    std::string constraint_;
    std::size_t target_where_{no_db_node};
    std::vector<db_stored_assignment> assignments_;
    std::size_t update_where_{no_db_node};
    bool do_nothing_{false};
    bool any_unique_key_{false};
};
struct db_stored_lock final {
    db_stored_lock(const db_lock_options& source_value)
        : mode_(source_value.mode_),
          nowait_(source_value.nowait_),
          skip_locked_(source_value.skip_locked_) {
        tables_.reserve(source_value.tables_.size());
        for (const auto& table : source_value.tables_) {
            tables_.emplace_back(table);
        }
    }
    db_stored_lock(const db_stored_lock&) = default;
    db_stored_lock& operator=(const db_stored_lock&) = default;
    db_stored_lock(db_stored_lock&&) noexcept = default;
    db_stored_lock& operator=(db_stored_lock&&) noexcept = default;
    db_row_lock mode_;
    bool nowait_{false};
    bool skip_locked_{false};
    std::vector<std::string> tables_;
};

class db_query_storage final {
public:
    db_query_storage() = default;
    db_query_storage(const db_query_storage& source);

    std::vector<db_query_node> nodes_;
    std::vector<db_query> queries_;
    db_query_kind kind_{db_query_kind::select};
    std::string target_;
    std::string target_alias_;
    std::vector<std::string> columns_;
    std::vector<std::size_t> projections_;
    std::vector<std::size_t> groups_;
    std::vector<db_stored_order> orders_;
    bool distinct_{false};
    std::vector<std::size_t> distinct_on_;
    std::vector<std::size_t> returning_;
    std::optional<db_query_source> source_{};
    std::vector<db_stored_join> joins_;
    std::vector<std::vector<std::size_t>> rows_;
    std::vector<db_stored_assignment> assignments_;
    std::size_t insert_query_{no_db_node};
    std::size_t predicate_{no_db_node};
    std::size_t having_{no_db_node};
    std::optional<std::uint64_t> limit_{};
    std::optional<std::uint64_t> offset_{};
    std::optional<db_stored_conflict> conflict_{};
    std::optional<db_stored_lock> lock_{};
    std::vector<db_stored_cte> ctes_;
    std::vector<db_stored_set_operation> set_operations_;

    std::optional<bool> cache_enabled_{};
    std::optional<std::chrono::milliseconds> cache_duration_{};
    std::string cache_id_;
    // The result shape a repository builder derived this query as; with
    // cache_id_ it names the id-keyed entry.
    db_cache_shape cache_shape_{db_cache_shape::rows};

    [[nodiscard]] bool returns_rows() const noexcept {
        return kind_ == db_query_kind::select || kind_ == db_query_kind::values || !returning_.empty();
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Readiness classes a database driver can wait for. A wait completes on the
// first class that becomes ready.
enum class db_socket_readiness : std::uint8_t {
    none = 0,
    read = 1,
    write = 2,
    error = 4,
};

[[nodiscard]] constexpr db_socket_readiness operator|(db_socket_readiness left, db_socket_readiness right) noexcept {
    return static_cast<db_socket_readiness>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool has_db_socket_readiness(db_socket_readiness set, db_socket_readiness value) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(value)) != 0;
}

struct db_socket_wait_result final {
    // The class that completed first; none when the wait failed or was cancelled.
    db_socket_readiness ready_{db_socket_readiness::none};
    std::error_code error_{};
    // The slot deadline guarding this wait expired.
    bool expired_{false};
};

// Ruvia-owned duplicate of a database driver's connection socket, registered
// with ASIO for readiness waits.
//
// MariaDB and libpq own their native socket and close it themselves inside
// their read/write calls when the transport fails (libmariadb end_server,
// libpq pqDropConnection). Registering that handle directly would leave ASIO
// holding a handle the driver may close -- and the OS may reuse -- between two
// waits. A duplicate refers to the same socket object, so it observes the same
// readiness, but its handle belongs to Ruvia: the driver closing its own
// handle can neither invalidate nor be invalidated by it. The duplicate is
// therefore made and registered once per driver socket and kept until the
// pool closes the connection.
//
// The duplicate keeps the underlying connection open after the driver has
// closed its handle. The driver only does that on a transport failure, which
// the pool answers by closing the slot, and closing the slot closes the
// duplicate first so the driver's own close still ends the connection.
struct db_slot_socket final {
    explicit db_slot_socket(asio::io_context& io_context);

    db_slot_socket(const db_slot_socket&) = delete;
    db_slot_socket& operator=(const db_slot_socket&) = delete;

#if defined(_WIN32)
    using native_socket_type = std::uintptr_t;
    using waitable_type = asio::ip::tcp::socket;
    asio::ip::tcp::socket socket_;
    static constexpr native_socket_type invalid_socket = std::numeric_limits<native_socket_type>::max();
#else
    using native_socket_type = asio::posix::stream_descriptor::native_handle_type;
    using waitable_type = asio::posix::stream_descriptor;
    asio::posix::stream_descriptor descriptor_;
    static constexpr native_socket_type invalid_socket = -1;
#endif
    // The driver handle the open duplicate was made from.
    native_socket_type source_{invalid_socket};

    // Any-of readiness wait. The first completion cancels the others, and the
    // awaiting coroutine resumes only after every started handler has run, so
    // no outstanding operation still borrows the awaiter.
    class readiness_awaiter final {
    public:
        readiness_awaiter(db_slot_socket& socket, db_socket_readiness wanted) noexcept
            : socket_(socket),
              wanted_(wanted) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) noexcept;
        // Rethrows a failure to start a wait.
        [[nodiscard]] db_socket_wait_result await_resume() const;

    private:
        void start(waitable_type::wait_type wait_type, db_socket_readiness readiness);
        void complete(db_socket_readiness readiness, std::error_code error) noexcept;

        db_slot_socket& socket_;
        db_socket_readiness wanted_;
        std::coroutine_handle<> continuation_{};
        db_socket_wait_result result_{};
        std::exception_ptr initiation_failure_{};
        std::uint8_t pending_{0};
        bool completed_{false};
    };

    // Duplicates and registers the driver's socket unless the open duplicate
    // was already made from this handle value. The value is the only identity
    // checked, so a caller must close() before the driver can replace its
    // socket with one that reuses the value.
    [[nodiscard]] std::error_code attach(native_socket_type driver_socket) noexcept;
    [[nodiscard]] readiness_awaiter wait(db_socket_readiness wanted) noexcept {
        return readiness_awaiter(*this, wanted);
    }
    void cancel() noexcept;
    // Cancels pending waits and closes the duplicate, leaving the driver's
    // socket open and unregistered.
    void close() noexcept;

private:
    [[nodiscard]] waitable_type& waitable() noexcept;
};

// Puts a driver-owned socket into non-blocking mode. A driver whose
// asynchronous API suspends on EAGAIN cannot suspend at all while its socket
// blocks, so it runs the whole operation inside the call that was supposed to
// start it.
[[nodiscard]] std::error_code set_db_socket_non_blocking(db_slot_socket::native_socket_type driver_socket) noexcept;

// What a look at an idle pooled connection's socket finds.
enum class db_idle_socket_status : std::uint8_t {
    quiet,
    // Bytes wait to be read.
    pending_data,
    // End of stream or a socket error: the server ended the session.
    closed,
};

// Peeks at most one byte of a driver-owned socket without blocking and
// without taking it from the driver (Pingora's test_reusable_stream). Both
// drivers keep their socket non-blocking: libpq always, MariaDB through
// set_db_socket_non_blocking().
[[nodiscard]] db_idle_socket_status peek_db_idle_socket(db_slot_socket::native_socket_type driver_socket) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

class validated_db_config_view final {
public:
    [[nodiscard]] const db_config& get() const noexcept {
        return *config_;
    }

private:
    friend validated_db_config_view validated_db_config(const db_config& config);

    explicit validated_db_config_view(const db_config& config) noexcept
        : config_(&config) {}

    const db_config* config_;
};

[[nodiscard]] inline constexpr std::uint16_t default_db_port(db_driver driver) noexcept {
    switch (driver) {
        case db_driver::mariadb:
            return 3306;
        case db_driver::postgresql:
            return 5432;
        default:
            return 0;
    }
}

[[nodiscard]] inline std::uint16_t configured_db_port(const db_config& config) noexcept {
    return config.port_.has_value() ? config.port_->value() : default_db_port(config.driver_);
}

inline void validate_db_config(const db_config& config) {
    validate_client_tls_config(config.tls_);
    // Both drivers are configured with key files only; refuse a password they
    // would silently ignore.
    if (config.tls_.identity_.has_value() && !config.tls_.identity_->private_key_password_.empty()) {
        throw std::invalid_argument("database TLS does not support encrypted private keys");
    }
    const auto driver = config.driver_;
    if (driver == db_driver::mariadb && config.tls_.mode_ == client_tls_mode::verify_identity) {
        if (!is_client_ip_address(config.host_) || config.tls_.server_name_.has_value()) {
            throw std::invalid_argument("MariaDB authenticated TLS requires a numeric host with an IP certificate; server_name is unsupported");
        }
    }
    if (driver != db_driver::mariadb && driver != db_driver::postgresql) {
        throw std::invalid_argument("database driver must be selected");
    }
#ifndef RUVIA_ENABLE_MARIADB
    if (driver == db_driver::mariadb) {
        throw std::invalid_argument("MariaDB support is not enabled");
    }
#endif
#ifndef RUVIA_ENABLE_POSTGRESQL
    if (driver == db_driver::postgresql) {
        throw std::invalid_argument("PostgreSQL support is not enabled");
    }
#endif

    validate_client_origin_host(config.host_, "database host must not be empty", "database host is invalid");
    // Both drivers pass these to C APIs as NUL-terminated strings.
    for (const std::string_view value : {std::string_view{config.username_}, std::string_view{config.password_},
             std::string_view{config.database_}}) {
        if (value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("database credentials must not contain NUL bytes");
        }
    }
    detail::ensure_non_zero_port(configured_db_port(config), "database port must not be zero");
    detail::ensure_positive_size(config.pool_size_per_worker_, "database pool size must be greater than zero");
    detail::ensure_positive_optional_durations("configured database timeouts must be greater than zero",
        config.connect_timeout_, config.read_timeout_, config.write_timeout_, config.query_timeout_,
        config.acquire_timeout_, config.transaction_timeout_);
}

[[nodiscard]] inline validated_db_config_view validated_db_config(const db_config& config) {
    validate_db_config(config);
    return validated_db_config_view(config);
}

validated_db_config_view validated_db_config(db_config&&) = delete;
validated_db_config_view validated_db_config(const db_config&&) = delete;

#ifdef RUVIA_ENABLE_REDIS
struct db_cache_config_storage final {
    template <typename config_type>
    db_cache_config_storage(const config_type& source_value)
        : duration_(source_value.duration_),
          always_enabled_(source_value.always_enabled_),
          ignore_errors_(source_value.ignore_errors_),
          key_prefix_(source_value.key_prefix_) {
        if (duration_.count() <= 0 || key_prefix_.empty()) {
            throw std::invalid_argument("database cache requires a positive duration and nonempty namespace");
        }
    }
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    std::string key_prefix_;
};
#endif

// Worker/app-owned copy of the public startup configuration. Retained runtime
// state is explicitly rebound to its owning PMR domain here.
struct db_config_storage final {
    db_config_storage(const db_config& source_value)
        : db_config_storage(validated_db_config(source_value)) {}

    db_config_storage(validated_db_config_view source_value)
        : db_config_storage(validated_config_tag_type{}, source_value.get()) {}

    db_config_storage(db_config_storage&&) noexcept = default;
    db_config_storage(const db_config_storage& source_value)
        : db_config_storage(validated_config_tag_type{}, source_value) {}

    db_driver driver_{db_driver::unspecified};
    std::string host_;
    std::uint16_t port_{0};
    std::string username_;
    std::string password_;
    client_tls_config_storage tls_;
    std::string database_;
    std::size_t pool_size_per_worker_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::optional<timeout_duration> query_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<timeout_duration> transaction_timeout_;
    // Whether a slot whose connection ended connects again on its next use.
    // Off for a pool bound to the sessions it opened at startup (the
    // migrator's): a statement on a lost session then fails rather than run
    // on a new one that lacks the old one's session-scoped state.
    bool reconnect_{true};

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static std::uint16_t resolved_port(const db_config& source) noexcept {
        return configured_db_port(source);
    }
    [[nodiscard]] static std::uint16_t resolved_port(const db_config_storage& source) noexcept {
        return source.port_;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config&) noexcept {
        return true;
    }
    [[nodiscard]] static bool resolved_reconnect(const db_config_storage& source) noexcept {
        return source.reconnect_;
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config& source) {
        return {source.tls_, client_tls_files::keep_paths};
    }
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const db_config_storage& source) {
        return {source.tls_};
    }

    template <typename source_type>
        requires(std::same_as<source_type, db_config> || std::same_as<source_type, db_config_storage>)
    db_config_storage(validated_config_tag_type, const source_type& source_value)
        : driver_(source_value.driver_),
          host_(source_value.host_),
          port_(resolved_port(source_value)),
          username_(source_value.username_),
          password_(source_value.password_),
          tls_(tls_storage(source_value)),
          database_(source_value.database_),
          pool_size_per_worker_(source_value.pool_size_per_worker_),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          query_timeout_(source_value.query_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          transaction_timeout_(source_value.transaction_timeout_),
          reconnect_(resolved_reconnect(source_value)) {}
};

#ifdef RUVIA_ENABLE_REDIS
struct db_query_cache_registration_storage final {
    db_query_cache_registration_storage(const db_query_cache_registration& source_value)
        : redis_alias_(source_value.redis_alias_),
          policy_(source_value.policy_) {
        validate_capability_alias(redis_alias_, "database query cache Redis alias must not be empty");
    }
    std::string redis_alias_;
    db_cache_config_storage policy_;
};
#endif

struct db_definition final {
    using config_storage_type = db_config_storage;
    std::string alias_;
    db_config_storage config_;
#ifdef RUVIA_ENABLE_REDIS
    std::optional<db_query_cache_registration_storage> query_cache_{};
#endif
};

}  // namespace ruvia::detail

// Pipelining of buffered statements outside transactions, shared by the
// drivers whose protocol lets one connection carry several statements at
// once. A statement joins a pipeline group: one leased connection that
// several statements share, each sending its own flight so that a failed
// statement affects no other. Results are read in send order by one member at
// a time, the reader, which hands each finished statement's outcome to its
// member and passes the role on when its own statement completes. Members wait
// in their own coroutine frames; the group's bookkeeping is owned by the
// connection's slot and the pool, because a member that is cancelled leaves
// before its results arrive.
//
// A driver supplies an io_type with its protocol: what a flight and a group
// keep (flight_type, group_state), the statement its caller prepared
// (statement_type), and the hooks that open a group, queue a flight, read
// results without waiting, wait for and exchange I/O, and judge whether a
// connection can be reused once its group dissolves. Everything else --
// admission in arrival order, room, the reader role, deadlines, cancellation
// and connection failure -- is this one implementation.
//
// A statement that changes session state (SET, USE, BEGIN, LOCK, ...) changes
// it for every statement sharing the connection after it; such statements
// belong in a transaction. A group dissolving with its connection left inside
// a transaction block closes that connection.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
template <typename io_type>
class db_pipeline_state;
template <typename io_type>
struct db_pipeline_record;

// Statements one connection keeps in flight at once. Locally, 16 reached about
// six times the throughput of one statement per round trip on PostgreSQL, 8
// somewhat less and 32 no more; a deeper pipeline only adds queueing latency.
inline constexpr std::size_t db_pipeline_depth = 16;

enum class db_pipeline_phase : std::uint8_t {
    // Holds nothing.
    admitting,
    // Waits in the pool's arrival order for room in a group or a connection.
    parked,
    // Holds a lease and founds a group on it.
    leased,
    // A member holding room for a flight it has not sent yet.
    reserved,
    // A member whose flight is in the group.
    pending,
    // A member whose statement has an outcome, or was abandoned.
    settled,
};

// What woke a waiting member.
enum class db_pipeline_event : std::uint8_t {
    none,
    // Admission: a connection was leased for it, or room in a group reserved.
    leased,
    joined,
    // It reads the group's results now.
    reader,
    // Its statement finished; failure_ tells how.
    completed,
    // Its statement must be sent once more (a stale cached statement).
    retry,
    // Its group's connection failed with failure_.
    failed,
    timed_out,
    cancelled,
    closing,
};

// How the head flight of a group ended, as the driver read it.
struct db_pipeline_outcome final {
    std::exception_ptr failure_{};
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    // The statement runs once more instead of completing.
    bool retry_{false};
};

// One pipelined statement. It lives in its caller's coroutine frame; the
// group refers to it only while it is a member, through records that forget
// it when it leaves. Construction binds its stop token and makes it findable
// for cancellation; destruction gives back whatever it still holds -- a parked
// position, a lease, room or a flight in a group -- so every exit path of the
// statement shares one cleanup.
template <typename io_type>
class db_pipeline_member final {
public:
    using state_type = db_pipeline_state<io_type>;

    db_pipeline_member(
        state_type& state, db_rows* rows, const stop_token& stop_token_value);
    ~db_pipeline_member();

    db_pipeline_member(const db_pipeline_member&) = delete;
    db_pipeline_member& operator=(const db_pipeline_member&) = delete;

    // Suspends until an event arrives, unless one already has.
    struct wait final {
        db_pipeline_member& member_;

        [[nodiscard]] bool await_ready() const noexcept {
            return member_.event_ != db_pipeline_event::none;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            member_.continuation_ = handle;
        }
        [[nodiscard]] db_pipeline_event await_resume() noexcept {
            return member_.take_event();
        }
    };

    [[nodiscard]] db_pipeline_event take_event() noexcept {
        return std::exchange(event_, db_pipeline_event::none);
    }
    // Its stop token was triggered, even if the cancellation has not been
    // dispatched yet.
    [[nodiscard]] bool stopped() const noexcept {
        return cancel_requested_ || stop_token_.stop_requested();
    }

    state_type& state_;
    // The caller's result for query(); null for execute().
    db_rows* rows_;
    // The caller's options outlive the statement's frame.
    const stop_token& stop_token_;
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_{};
    std::exception_ptr failure_{};
    // The statement deadline, from its first send.
    std::optional<std::chrono::steady_clock::time_point> deadline_{};
    std::coroutine_handle<> continuation_{};
    // The record of its flight while pending.
    db_pipeline_record<io_type>* record_{nullptr};
    // Its place in the pool's arrival order, shared with the callers waiting
    // for a lease.
    std::uint64_t arrival_{0};
    std::size_t slot_{0};
    db_pipeline_phase phase_{db_pipeline_phase::admitting};
    db_pipeline_event event_{db_pipeline_event::none};
    // Set when its cancellation is dispatched, which also unlinks it from the
    // statements a cancellation searches.
    bool cancel_requested_{false};
    bool connecting_{false};
    bool may_retry_{true};
    // The pool's FIFO of parked statements.
    db_pipeline_member* previous_parked_{nullptr};
    db_pipeline_member* next_parked_{nullptr};
    // The pool's cancellable statements, newest first.
    db_pipeline_member* previous_{nullptr};
    db_pipeline_member* next_{nullptr};
    worker_timer_registration admission_timer_;
    std::uint64_t cancellation_id_{0};
    worker_cancellation_registration<worker_cancellation_target<state_type>> cancellation_;
};

// One flight in a group, in send order.
template <typename io_type>
struct db_pipeline_record final {
    // Null once the member abandoned the statement; its results are discarded.
    db_pipeline_member<io_type>* member_{nullptr};
    typename io_type::flight_type flight_{};
    bool returns_rows_{false};
};

// The pipeline group of one connection; it exists while it has members. The
// records form a ring of the flights in send order; the head is the one whose
// results are read next.
template <typename io_type>
struct db_pipeline_group final {
    [[nodiscard]] db_pipeline_record<io_type>& record(std::size_t offset) noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    [[nodiscard]] const db_pipeline_record<io_type>& record(std::size_t offset) const noexcept {
        return records_[(head_ + offset) % db_pipeline_depth];
    }
    // Its oldest flight was abandoned: a statement sent now would wait behind
    // one whose caller already gave up on it, so the group takes nobody new
    // until that flight's results have been read.
    [[nodiscard]] bool stalled() const noexcept {
        return pending_ != 0 && records_[head_].member_ == nullptr;
    }

    std::array<db_pipeline_record<io_type>, db_pipeline_depth> records_{};
    // The driver's protocol state of the connection's flights.
    typename io_type::group_state io_{};
    // Why the connection failed, once closing_.
    std::exception_ptr failure_{};
    // When the reader's current socket wait ends at the latest.
    std::optional<std::chrono::steady_clock::time_point> wait_deadline_{};
    // Start of the connection's current stretch without progress -- no socket
    // readiness and no flight completed -- while flights are outstanding. The
    // I/O timeout counts from it whichever member holds the reader role.
    std::optional<std::chrono::steady_clock::time_point> stretch_start_{};
    db_pipeline_member<io_type>* reader_{nullptr};
    std::size_t head_{0};
    std::size_t pending_{0};
    // Room promised to members that have not sent their flight yet.
    std::size_t reserved_{0};
    std::size_t members_{0};
    // The connection failed; the group takes no members and dissolves.
    bool closing_{false};
};

// Per pool: admission into groups, in arrival order with the callers waiting
// for a lease. A statement founds a group on a free connection, else joins
// the group with the most room, else parks in a FIFO served by freed room and
// by released connections. A statement never joins a group ahead of an older
// caller still waiting for a lease: the groups drain to it, and a released
// connection goes to it. A parked statement older than every caller waiting
// for a lease takes a released connection itself, before the lease scheduler
// can hand it on.
template <typename io_type>
class db_pipeline_state final {
public:
    using driver_type = typename io_type::driver_type;
    using pool_type = db_pool<driver_type>;
    using member_type = db_pipeline_member<io_type>;
    using group_type = db_pipeline_group<io_type>;
    using record_type = db_pipeline_record<io_type>;
    using statement_type = typename io_type::statement_type;
    using cancellation_target_type = worker_cancellation_target<db_pipeline_state>;

    db_pipeline_state(pool_type& pool, const worker_ref& worker_value);
    ~db_pipeline_state();

    db_pipeline_state(const db_pipeline_state&) = delete;
    db_pipeline_state& operator=(const db_pipeline_state&) = delete;

    // db_pool hooks.
    [[nodiscard]] bool take_released_slot(std::size_t slot) noexcept;
    void exclusive_waiter_left() noexcept;
    // Fails every parked statement with closing.
    void close() noexcept;
    // worker_cancellation_target hook.
    void cancel_operation_by_id(std::uint64_t cancellation_id) noexcept;

    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target() const noexcept {
        return cancellation_target_;
    }

    // The statement's way through a group, in order. admit() places it, or
    // parks it to be woken with the event admitted() checks. A statement that
    // leased a connection connects it if needed and founds a group on it.
    [[nodiscard]] db_pipeline_event admit(member_type& member, const operation_deadline& admission_deadline);
    void admitted(db_pipeline_event event) const;
    [[nodiscard]] bool must_connect(member_type& member);
    void connected(member_type& member, const std::exception_ptr& failure);
    void found_group(member_type& member);
    // Queues the member's flight in its reserved room; it becomes the reader
    // when the group has none.
    void send(member_type& member, statement_type& statement);
    [[nodiscard]] bool reads(const member_type& member) const noexcept;
    // Turns the event that ended the member's wait into its outcome: false
    // once it completed, true when it must be sent once more; throws its
    // failure.
    [[nodiscard]] bool retries(member_type& member, db_pipeline_event event) const;

    // Services for the reader. read_buffered() reads the results available
    // without waiting and returns true once the reader's own statement
    // finished, after handing the role on.
    [[nodiscard]] bool read_buffered(member_type& reader);
    // Abandons the pending followers whose deadline passed.
    void expire_followers(member_type& reader, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> earliest_deadline(std::size_t slot) const noexcept;
    // Fails every pending member and closes the connection.
    void fail_group(std::size_t slot, const std::exception_ptr& failure) noexcept;
    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    // Services for the driver's hooks.
    [[nodiscard]] pool_type& pool() const noexcept {
        return pool_;
    }
    // Statements this pool runs now, in every phase from admission on.
    [[nodiscard]] std::size_t statements() const noexcept {
        return statements_;
    }
    [[nodiscard]] group_type& group(std::size_t slot) const noexcept;
    // Hands the head flight's outcome to its member, which the reader settles
    // itself and wakes any other; an abandoned flight's outcome is discarded.
    void complete_head(std::size_t slot, member_type& reader, db_pipeline_outcome outcome);
    [[nodiscard]] db_error cancelled() const;

private:
    friend class db_pipeline_member<io_type>;

    // Whether a statement of this arrival may take a connection or room: no
    // older caller still waits for a lease.
    [[nodiscard]] bool may_share(std::uint64_t arrival) const noexcept;
    // Places a statement whose turn it is: a free connection, else room in the
    // group with the most of it that is not stalled.
    [[nodiscard]] db_pipeline_event place(member_type& member);
    [[nodiscard]] std::optional<std::size_t> roomiest_group() const noexcept;
    void join(member_type& member, std::size_t slot) noexcept;
    void park(member_type& member, const operation_deadline& admission_deadline);
    void unpark(member_type& member) noexcept;
    // Serves parked statements in arrival order.
    void dispatch() noexcept;
    // Records what woke a member and posts its resumption.
    void settle(member_type& member, db_pipeline_event event) noexcept;
    void require_usable(std::size_t slot) const;
    void hand_off(member_type& reader) noexcept;
    void abandon(member_type& member) noexcept;
    void clear_flights(group_type& cleared) noexcept;
    void unlink(member_type& member) noexcept;
    void retire(member_type& member) noexcept;
    void leave(member_type& member) noexcept;
    void dissolve(std::size_t slot) noexcept;

    pool_type& pool_;
    const worker_ref& worker_;
    member_type* members_{nullptr};
    std::size_t statements_{0};
    member_type* parked_head_{nullptr};
    member_type* parked_tail_{nullptr};
    std::shared_ptr<cancellation_target_type> cancellation_target_;
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// Names one entry of a db_statement_cache by position and generation, so
// forgetting or replacing an entry never redirects another statement's handle
// to a different statement; a stale handle finds nothing.
struct db_statement_entry final {
    std::size_t index_;
    std::uint64_t generation_;
};

// The server-side prepared statements of one connection, shared by the drivers
// that cache them. A statement is keyed by its SQL and the type codes of its
// parameters, since a server fixes parameter (and with them result) types when
// it prepares. A statement is prepared on its second use: the first runs
// unprepared, so SQL that never repeats (literals, generated lists) costs no
// more than without the cache and does not evict reused statements. Entries
// live as long as the connection; the least recently used one makes room for
// a new one, and its payload goes back to the driver to release on the server.
template <typename payload_type>
class db_statement_cache final {
public:
    static constexpr std::size_t capacity = 128;

    using entry_ref = db_statement_entry;

    struct lookup final {
        // Absent: run the statement unprepared.
        std::optional<entry_ref> statement_{};
        // The statement is new to the connection: prepare it first.
        bool inserted_{false};
        // The payload of an entry evicted to make room.
        std::optional<payload_type> evicted_{};
        // The statement's key_hash().
        std::size_t hash_{0};
        // Not cached, used before, and not inserted only because the caller
        // may not insert it now.
        bool repeated_{false};
    };

    db_statement_cache()
        : entries_() {}

    // Counts a use of the statement. A cached one is returned. One used
    // before is inserted when the caller `may_insert` it now, with the payload
    // make() builds; the whole entry is built before the table changes, so a
    // failed allocation leaves every entry intact. Otherwise the use is
    // remembered and nothing returned.
    template <typename factory_type>
    [[nodiscard]] lookup find(
        std::string_view sql, std::span<const unsigned int> types, bool may_insert, factory_type&& make) {
        const auto hash = key_hash(sql, types);
        ++clock_;
        const auto cached = entry_index_.find(hash, [&](std::size_t index) noexcept {
            const auto& existing = entries_[index];
            return existing.generation_ != 0 && existing.hash_ == hash && existing.sql_ == sql &&
                   std::ranges::equal(existing.types_, types);
        });
        if (cached.has_value()) {
            auto& existing = entries_[*cached];
            existing.last_use_ = clock_;
            return {entry_ref{*cached, existing.generation_}, false, std::nullopt, hash};
        }
        const auto first_use_index = find_first_use(hash);
        if (!first_use_index.has_value()) {
            remember(hash);
            return {std::nullopt, false, std::nullopt, hash};
        }
        if (!may_insert) {
            return {std::nullopt, false, std::nullopt, hash, true};
        }
        const auto free_entry = std::ranges::find(entries_, std::uint64_t{0}, &entry::generation_);
        const bool has_free_entry = free_entry != entries_.end();
        if (!has_free_entry && entries_.size() < capacity && entries_.size() == entries_.capacity()) {
            entries_.reserve(std::min(capacity, std::max<std::size_t>(entries_.size() * 2, 8)));
        }
        std::string key_sql(sql);
        std::vector<unsigned int> key_types(types.begin(), types.end());
        std::optional<payload_type> created_payload;
        if constexpr (std::is_invocable_v<factory_type, std::string_view, std::span<const unsigned int>,
                          std::size_t>) {
            created_payload = make(sql, types, hash);
            if (!created_payload.has_value()) {
                return {std::nullopt, false, std::nullopt, hash, true};
            }
        } else {
            created_payload.emplace(make(sql, types));
        }
        // Nothing below allocates: the key was copied above, `entries_` has
        // room, and every string and vector shares one resource, so swaps and
        // moves only exchange storage.
        const auto next_generation = generation_ + 1;
        auto index = entries_.size();
        std::optional<payload_type> evicted;
        if (has_free_entry || index == capacity) {
            if (has_free_entry) {
                // A forgotten entry is reused before the table grows.
                index = static_cast<std::size_t>(free_entry - entries_.begin());
            } else {
                // Only a full table evicts a statement the server still holds.
                const auto oldest = std::ranges::min_element(entries_, {}, &entry::last_use_);
                index = static_cast<std::size_t>(oldest - entries_.begin());
                entry_index_.erase(index, oldest->hash_, [this](std::size_t position) noexcept { return entries_[position].hash_; });
                evicted.emplace(std::move(oldest->payload_));
            }
            auto& reused = entries_[index];
            reused.sql_.swap(key_sql);
            reused.types_.swap(key_types);
            reused.hash_ = hash;
            reused.last_use_ = clock_;
            reused.generation_ = next_generation;
            reused.payload_ = std::move(*created_payload);
        } else {
            entries_.push_back(
                entry{std::move(key_sql), std::move(key_types), hash, clock_, next_generation, std::move(*created_payload)});
        }
        entry_index_.insert(index, hash);
        clear_first_use(*first_use_index, hash);
        ++generation_;
        return {entry_ref{index, next_generation}, true, std::move(evicted), hash};
    }

    [[nodiscard]] payload_type* payload(entry_ref statement) noexcept {
        auto* found = live(statement);
        return found == nullptr ? nullptr : &found->payload_;
    }

    // The SQL of a live entry, owned by the cache. It stays in place until the
    // next find() or clear().
    [[nodiscard]] std::string_view sql(entry_ref statement) const noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return {};
        }
        return entries_[statement.index_].sql_;
    }

    // Drops an entry the server does not hold (a failed prepare) or no longer
    // holds as cached (a stale statement), unless it was replaced since, and
    // hands its payload back. With `use_again` its next use prepares it
    // straight away, as for a statement used before.
    std::optional<payload_type> forget(entry_ref statement, bool use_again) noexcept {
        auto* found = live(statement);
        if (found == nullptr) {
            return std::nullopt;
        }
        const auto old_hash = found->hash_;
        entry_index_.erase(statement.index_, old_hash, [this](std::size_t index) noexcept { return entries_[index].hash_; });
        found->generation_ = 0;
        if (use_again) {
            remember(old_hash);
        }
        return std::move(found->payload_);
    }

    // Calls visit() with the payload of every live entry.
    template <typename visitor_type>
    void for_each(visitor_type&& visit) {
        for (auto& existing : entries_) {
            if (existing.generation_ != 0) {
                visit(existing.payload_);
            }
        }
    }

    // A new connection starts without server-side statements.
    void clear() noexcept {
        entries_.clear();
        entry_index_.clear();
        first_uses_.fill(0);
        first_use_index_.clear();
    }

    // The hash a key is kept under; never zero, which marks a free position.
    [[nodiscard]] static std::size_t key_hash(std::string_view sql, std::span<const unsigned int> types) noexcept {
        auto hash = std::hash<std::string_view>{}(sql) ^ types.size();
        for (const auto type : types) {
            hash = (hash ^ type) * fnv1a_prime;
        }
        return hash == 0 ? 1 : hash;
    }

private:
    struct entry final {
        std::string sql_;
        std::vector<unsigned int> types_;
        std::size_t hash_;
        std::uint64_t last_use_;
        // Zero marks a forgotten entry, free for reuse. Never reused across
        // clear(), so no handle outlives its entry unnoticed.
        std::uint64_t generation_;
        payload_type payload_;
    };

    [[nodiscard]] entry* live(entry_ref statement) noexcept {
        if (statement.index_ >= entries_.size() || entries_[statement.index_].generation_ != statement.generation_) {
            return nullptr;
        }
        return &entries_[statement.index_];
    }

    static constexpr std::size_t index_capacity = 256;
    static_assert((index_capacity & (index_capacity - 1)) == 0 && index_capacity >= capacity * 2);

    // An open-addressing index over at most `capacity` positions, stored as
    // position + 1 so zero marks an empty slot: probed linearly from the
    // hash, deleted by backward shift. `hash_of(position)` reads the hash a
    // stored position was inserted under.
    class probe_index final {
    public:
        template <typename match_type>
        [[nodiscard]] std::optional<std::size_t> find(std::size_t hash, match_type&& match) const noexcept {
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return std::nullopt;
                }
                if (const auto position = static_cast<std::size_t>(code - 1); match(position)) {
                    return position;
                }
            }
            return std::nullopt;
        }

        void insert(std::size_t position, std::size_t hash) noexcept {
            std::size_t slot = hash & mask;
            while (slots_[slot] != 0) {
                slot = (slot + 1) & mask;
            }
            slots_[slot] = static_cast<std::uint8_t>(position + 1);
        }

        template <typename hash_of_type>
        void erase(std::size_t position, std::size_t hash, hash_of_type&& hash_of) noexcept {
            const auto target = static_cast<std::uint8_t>(position + 1);
            for (std::size_t step = 0, slot = hash & mask; step < index_capacity; ++step, slot = (slot + 1) & mask) {
                const auto code = slots_[slot];
                if (code == 0) {
                    return;
                }
                if (code == target) {
                    std::size_t empty = slot;
                    for (std::size_t next = (slot + 1) & mask; slots_[next] != 0; next = (next + 1) & mask) {
                        const auto ideal = hash_of(static_cast<std::size_t>(slots_[next] - 1)) & mask;
                        if (((next - ideal) & mask) >= ((next - empty) & mask)) {
                            slots_[empty] = slots_[next];
                            empty = next;
                        }
                    }
                    slots_[empty] = 0;
                    return;
                }
            }
        }

        void clear() noexcept {
            slots_.fill(0);
        }

    private:
        static constexpr std::size_t mask = index_capacity - 1;
        std::array<std::uint8_t, index_capacity> slots_{};
    };

    [[nodiscard]] std::optional<std::size_t> find_first_use(std::size_t hash) const noexcept {
        return first_use_index_.find(hash, [&](std::size_t position) noexcept { return first_uses_[position] == hash; });
    }

    void clear_first_use(std::size_t position, std::size_t hash) noexcept {
        first_use_index_.erase(position, hash, [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        first_uses_[position] = 0;
    }

    void remember(std::size_t hash) noexcept {
        if (find_first_use(hash).has_value()) {
            return;
        }
        const auto position = next_first_use_;
        if (first_uses_[position] != 0) {
            first_use_index_.erase(position, first_uses_[position], [this](std::size_t stored) noexcept { return first_uses_[stored]; });
        }
        first_uses_[position] = hash;
        first_use_index_.insert(position, hash);
        next_first_use_ = (next_first_use_ + 1) % first_uses_.size();
    }

    std::vector<entry> entries_;
    probe_index entry_index_;
    // Key hashes of recent first uses; zero marks a free position. A hash
    // collision only prepares a statement on its first use.
    std::array<std::size_t, capacity> first_uses_{};
    probe_index first_use_index_;
    std::size_t next_first_use_{0};
    std::uint64_t clock_{0};
    std::uint64_t generation_{0};
};

}  // namespace ruvia::detail

struct st_mysql;
struct st_mysql_res;
struct ma_charset_info_st;

namespace ruvia::detail {

// Builds the error for a failed driver call from what libmariadb reports for
// it. A statement_failed whose cause ended the connection is reported as
// io_error.
[[nodiscard]] db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type code);
[[nodiscard]] db_error mysql_error(
    const st_mysql& connection, std::string_view operation, db_error::code_type code);

// A failed command after which the connection cannot carry another one: the
// client library failed or lost the exchange, or the server ended the session
// or read a packet it did not expect. The server answers any other error once
// the command is done, and the connection is ready for the next.
[[nodiscard]] bool mariadb_connection_failure(unsigned int native_code) noexcept;

// MariaDB's C API carries SQL and escaped-value lengths in unsigned long.
// Reject a wider C++ view before any narrowing conversion reaches the driver.
void validate_mariadb_sql_length(std::size_t length);

void free_stored_result(void* result_value) noexcept;

// The query that reads the session's client character set, and the
// character set its stored result names. The latter throws
// std::invalid_argument when libmariadb does not know it.
inline constexpr std::string_view mariadb_charset_query = "SELECT @@character_set_client";
[[nodiscard]] const ma_charset_info_st& mariadb_queried_charset(st_mysql_res& result);
// utf8mb4, the client character set a connection opens with: the one to
// escape for while the session's tracking says it is still in effect.
[[nodiscard]] const ma_charset_info_st& mariadb_utf8mb4_charset() noexcept;

// Writes `sql` with each placeholder replaced by its parameter's literal to
// `output`. A quote is escaped by doubling it, which reads the same in every
// sql_mode and character set; a backslash, in a session that reads backslash
// escapes, by another one, as `charset` (the session's client character set)
// splits the string into characters. The session's status must be current:
// every response before has been read whenever the statement or a string
// parameter has a backslash (mariadb_statement_classification::has_backslash_,
// mariadb_parameter_traits::has_backslash_); `charset` is needed only where
// mariadb_escape_needs_charset() says so. A string parameter with a NUL byte is
// refused: no SQL literal carries one.
void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::string& output, const ma_charset_info_st* charset = nullptr);

// What a statement's text tells about how it may run.
struct mariadb_statement_traits final {
    // A cached statement may run prepared: not a CALL, whose OUT parameters
    // come back as an extra result set only in the binary protocol; not a
    // LOAD; not a SET or USE, which change the session every cached
    // statement was prepared in and so would drop their own handle each
    // time they run; not one with RETURNING, whose changes a failure after
    // its result changed shape would hide; and no executable comment, whose
    // content the server runs but this scan cannot see.
    bool preparable_{false};
    // Placeholders, by position, where the text protocol takes only an
    // integer literal (LIMIT, OFFSET, FETCH and ROWS EXAMINED counts).
    std::uint64_t integer_params_{0};
};

[[nodiscard]] mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept;

struct mariadb_statement_classification final {
    // A statement that must have the connection to itself: LOAD ... LOCAL INFILE
    // answers its command with a request for the file, then reads the file's
    // content as packets of the same command, so a command sent behind it would
    // be read as file content. A statement led by an executable comment counts as
    // one, since the comment may hide a LOAD.
    bool exclusive_{false};
    // A statement that may run a stored procedure: a CALL, or one hidden behind
    // an executable comment.
    bool calls_routine_{false};
    // Whether `sql` names one of the session tracking variables.
    bool names_session_tracking_{false};
    // Whether `sql` contains a backslash character.
    bool has_backslash_{false};
};

struct mariadb_parameter_traits final {
    bool has_backslash_{false};
    // A backslash right after a byte above 0x7F.
    bool has_ambiguous_backslash_{false};
};

[[nodiscard]] mariadb_statement_classification classify_mariadb_statement(std::string_view sql) noexcept;
[[nodiscard]] mariadb_parameter_traits inspect_mariadb_parameters(std::span<const db_value> params) noexcept;
// Whether escaping string parameters for the session needs its client
// character set. Only an escape a string needs could change its meaning: a
// backslash put in front of its own backslash, in a session that reads
// backslash escapes (its current server status tells). That is safe in every
// character set right after an ASCII byte; right after another byte a
// multibyte character set (GBK, Big5, SJIS) may read the two as one character
// and leave the inserted backslash to escape what follows.
[[nodiscard]] bool mariadb_escape_needs_charset(
    const st_mysql& connection, bool has_ambiguous_backslash) noexcept;

}  // namespace ruvia::detail

struct st_mysql;
struct st_mysql_stmt;

namespace ruvia::detail {

// Storage a bound parameter points at for the length of an execution.
union mariadb_param_value {
    long long integer_;
    double real_;
    signed char flag_;
};

// Storage of one result column between mysql_stmt_fetch() and formatting.
struct mariadb_column final {
    std::vector<char> buffer_;
    unsigned long length_{0};
    char null_{0};
    char error_{0};
};

// One cached statement: its entry_ref once prepared, the placeholders its text
// restricts (mariadb_statement_traits::integer_params_), and the hash of its
// key, for refusing it.
struct mariadb_statement final {
    st_mysql_stmt* handle_{nullptr};
    std::uint64_t integer_params_{0};
    std::size_t key_hash_{0};
};

// Server-side prepared statements of one MariaDB connection, so a repeated
// statement skips the server's parse and travels in the binary protocol
// (db_statement_cache: keyed by SQL and parameter types, prepared on its
// second use, the least recently used one closed for room).
//
// The text protocol is the reference: a statement runs prepared only where
// the binary protocol returns the same field text and errors. So it runs as
// text when the server will not prepare it, when its statement text rules it
// out (mariadb_statement_traits), when a result column is one whose text the
// binary value cannot tell (a YEAR the prepare describes), and for a call whose
// parameters the server would read otherwise than their literals: a negative
// number (a literal's unary minus binds looser than COLLATE, and -? of
// INT64_MIN overflows only as a parameter), an integer above INT64_MAX, a
// string with a NUL byte or a non-finite double (refused by the text
// protocol), and anything but a non-negative integer where the text protocol
// takes only an integer literal (LIMIT ?).
//
// The server parses a statement when it prepares it, under the session's
// sql_mode, character sets, default schema and other variables of that
// moment, and keeps that reading. The session reports every change of those
// variables (mariadb_tracked_variables, tracked with the schema from when the
// connection opens): such a change drops every cached statement, so each is
// prepared anew under the new state; the session's other reports (last_gtid
// after each commit when binary logging is on, autocommit) leave the cache
// alone. A connection whose server does not track its session, whose
// tracking a statement turned off, or whose client or connection character
// set is not the utf8mb4 it opened with (the binary protocol converts
// parameters otherwise than the text protocol does literals) runs nothing
// prepared. A stored procedure may turn tracking off without a report, so
// after a CALL the next prepared execution first reads the tracked list back
// from the server.
//
// The cache owns every statement entry_ref, including those evicted or found
// stale that still wait to be closed on the server, and each entry's SQL,
// which libmariadb reads while a prepare is suspended. After the connection
// was closed, clear() frees the handles without I/O.
class mariadb_statement_cache final {
public:
    using entry_ref = db_statement_entry;

    mariadb_statement_cache();
    ~mariadb_statement_cache();

    mariadb_statement_cache(const mariadb_statement_cache&) = delete;
    mariadb_statement_cache& operator=(const mariadb_statement_cache&) = delete;

    // Counts a use of `sql` with `params`. A statement comes back only when
    // the caller may run it prepared now and it would return what its text
    // returns; another use still counts toward preparing it later.
    [[nodiscard]] std::optional<entry_ref> find(std::string_view sql, std::span<const db_value> params, bool may_prepare);
    // A new connection starts without server-side statements; the old one is
    // closed, so its handles are freed without I/O. Until connected() says
    // otherwise, the session counts as untracked.
    void clear() noexcept;

    // Hears the session changes `connection` reads from the server; false
    // when libmariadb refused the callback. Called before it connects.
    [[nodiscard]] bool observe(st_mysql& connection);
    // The connection opened: whether the server tracks every change of its
    // session for it from now on.
    void connected(bool tracked) noexcept;
    // A statement that may turn session tracking off (it names one of its
    // variables, whose change the server does not report) is about to run.
    void untrack() noexcept;
    // A statement that may run a stored procedure was sent; the procedure
    // may have turned tracking off unreported.
    void routine_called() noexcept;
    // Whether the session's client character set is known to be utf8mb4:
    // tracked, and so current once every response before has been read.
    [[nodiscard]] bool client_utf8mb4() const noexcept;

    [[nodiscard]] mariadb_statement& entry(entry_ref statement);
    void reserve_retired(std::size_t count = 1);
    void retire(st_mysql_stmt* statement) noexcept;
    void refuse(std::size_t hash) noexcept;
    void refuse(entry_ref statement, std::size_t hash) noexcept;
    [[nodiscard]] bool refused(std::size_t hash) const noexcept;
    void invalidate();
    [[nodiscard]] bool usable() const noexcept;
    void lose_tracking() noexcept;

    db_statement_cache<mariadb_statement> statements_;
    std::vector<st_mysql_stmt*> retired_;
    // Key hashes of statements that run as text on this connection; zero
    // marks a free position.
    std::array<std::size_t, db_statement_cache<mariadb_statement>::capacity> refused_{};
    std::size_t next_refused_{0};
    // What the session reported: whether it tracks every variable the cache
    // relies on and the schema, and whether its client and connection
    // character sets are still utf8mb4; and a change since the last find().
    bool tracked_{false};
    bool all_variables_{false};
    bool schema_tracked_{false};
    bool untracked_{false};
    bool client_utf8mb4_{true};
    bool connection_utf8mb4_{true};
    bool changed_{false};
    // A stored procedure ran since the tracked list was last read back.
    bool verify_{false};
    // Scratch of the one execution a connection runs at a time.
    std::vector<unsigned int> types_;
    std::vector<std::max_align_t> binds_;
    std::vector<mariadb_param_value> values_;
    std::vector<mariadb_column> columns_;
    // The bytes the column buffers hold, and those the last result needed.
    std::size_t column_bytes_{0};
    std::size_t column_needed_{0};
};

// The system variables a statement's parse or result depends on, as
// session_track_system_variables lists them, with the two tracking variables
// themselves, so that turning the schema's tracking off or changing this
// list to one that still names it is reported. A change of
// collation_connection is reported as one of character_set_connection.
inline constexpr std::string_view mariadb_tracked_variables =
    "sql_mode,old_mode,character_set_client,character_set_connection,character_set_results,time_zone,"
    "div_precision_increment,default_week_format,lc_time_names,max_sort_length,group_concat_max_len,"
    "session_track_system_variables,session_track_schema";

// Turns on the session tracking the statement cache relies on.
inline constexpr auto mariadb_session_tracking_storage = [] {
    constexpr std::string_view prefix = "SET SESSION session_track_system_variables = '";
    constexpr std::string_view suffix = "', session_track_schema = ON";
    std::array<char, prefix.size() + mariadb_tracked_variables.size() + suffix.size()> statement{};
    auto out = statement.begin();
    for (const auto part : std::array{prefix, mariadb_tracked_variables, suffix}) {
        for (const char byte : part) {
            *out++ = byte;
        }
    }
    return statement;
}();
inline constexpr std::string_view mariadb_session_tracking_setup(mariadb_session_tracking_storage.data(), mariadb_session_tracking_storage.size());

// One prepared execution as a sequence of libmariadb operations: after a
// stored procedure ran, reading back the session's tracked variables;
// closing the statements waiting to be closed, preparing the statement on its
// first prepared use, executing it, and storing each result.
// resume_mariadb_prepared() starts or continues the current operation and
// returns the wait mask its continuation needs; 0 hands the run back to its
// caller, which goes on according to step_.
struct mariadb_prepared_run final {
    enum class step : std::uint8_t {
        // Querying @@session_track_system_variables, then storing its row.
        verifying,
        verified,
        close,
        prepare,
        // The caller confirms the statement still runs, then resumes.
        execute,
        executing,
        storing,
        // The rows failed (the server's error ended them): back to the state
        // after prepare, so that the next execution starts clean.
        resetting,
        // The caller takes the stored rows, then resumes.
        rows,
        next_result,
        // Reading past the rows of a result the run cannot use.
        draining,
        done,
    };

    mariadb_statement_cache* cache_{nullptr};
    mariadb_statement_cache::entry_ref handle_{};
    std::span<const db_value> params_{};
    step step_{step::done};
    // The current step's operation has been started and waits to continue.
    bool started_{false};
    // A entry_ref being closed on the server, or drained.
    st_mysql_stmt* closing_{nullptr};
    // The statement's outcome: its failure, or what its first result reported.
    std::exception_ptr failure_{};
    std::exception_ptr output_failure_{};
    std::uint64_t affected_rows_{0};
    std::uint64_t insert_id_{0};
    bool returned_rows_{false};
    bool extra_row_set_{false};
    // It did not execute because the server no longer holds it as prepared
    // (ER_NEED_REPREPARE, ER_UNKNOWN_STMT_HANDLER). It was dropped from the
    // cache, and a run outside a transaction may go again, preparing it
    // anew. A statement whose result changed shape after it ran is dropped
    // as well, but fails: the server has run it, and running it again would
    // repeat its effects.
    bool stale_{false};
    // The server refused to prepare it, its result is not readable as text,
    // or the session no longer tracks what the cache relies on: run it as
    // text instead.
    bool text_{false};
};

// Starts a run for a statement find() returned.
[[nodiscard]] mariadb_prepared_run start_mariadb_prepared(
    mariadb_statement_cache& cache, mariadb_statement_cache::entry_ref statement, std::span<const db_value> params) noexcept;
// Continues a run with the readiness libmariadb waited for (0 to start the
// current step); returns the next wait mask, or 0 when the caller goes on.
// A failure that leaves the connection out of step is thrown.
[[nodiscard]] int resume_mariadb_prepared(st_mysql& connection, mariadb_prepared_run& run, int ready);
// Keeps the stored rows of the run's first result in `output`, or discards
// them when it is null; then the run resumes.
void take_mariadb_prepared_rows(mariadb_prepared_run& run, db_rows* output);
// Ends a run whose statement was abandoned before it executed.
void abandon_mariadb_prepared(mariadb_prepared_run& run) noexcept;
// What a statement's responses amount to, whichever protocol ran it: its own
// failure, a second row set, a failure to keep its rows, or a row set the
// caller did (not) ask for; null when it succeeded.
[[nodiscard]] std::exception_ptr mariadb_statement_failure(const std::exception_ptr& failure, bool extra_row_set,
    const std::exception_ptr& output_failure, bool returns_rows, bool returned_rows);

}  // namespace ruvia::detail

struct st_mysql_res;
struct ma_charset_info_st;

// MariaDB's part of statement pipelining (db_pipeline.h). A group's
// statements are sent as separate COM_QUERY commands (mysql_send_query) and
// their responses read in order (mysql_read_query_result and
// mysql_store_result). A failed statement does not affect the ones after it:
// the server runs each command on its own.
//
// libmariadb runs one nonblocking operation per connection at a time, and
// accepts a command only once every result before it has been fully read. So
// flights queue in the group and the reader sends them whenever it stands
// between two responses, before it reads the next one: statements that arrive
// while a response is being read go out right after it. The bytes sent ahead
// of unread responses are bounded (mariadb_pipeline_send_budget), so neither
// side can fill its socket buffers with requests while the other waits to
// write responses; a statement larger than the budget is sent once nothing is
// outstanding.
//
// A command is sent ahead of responses only when the statements before it
// cannot change what it means. Its parameters are interpolated when it is
// queued, in text every session reads alike; a statement whose interpolation
// depends on the session -- a backslash in its text or in a string parameter,
// which NO_BACKSLASH_ESCAPES reads either way -- is interpolated and sent once
// every response before it has been read, a SET of sql_mode among them, and
// when its escape depends on the session's client character set
// (mariadb_escape_needs_charset()), that is read from the server first, unless
// session tracking says it is still utf8mb4. A LOAD ... LOCAL INFILE
// (mariadb_statement_classification::exclusive_) goes out alone, and nothing
// follows it until its response has been read.
//
// A statement found in the connection's statement cache runs prepared only
// when it is the only statement its worker runs: the binary protocol has no
// nonblocking way to read an execution's response other than by waiting for
// it, so a prepared statement cannot share the wire with other flights, and
// waiting for each response costs more than the server saves on parsing once
// other statements compete for the worker. Those run as pipelined text.

namespace ruvia::detail {

template <typename driver_type>
class db_pool;
struct mariadb_driver;

inline constexpr std::size_t mariadb_pipeline_send_budget = 64 * 1024;

// One statement's command: its text, kept by its caller until it is sent,
// and the statement as written, for running it prepared or interpolating it
// late.
struct mariadb_flight final {
    // Empty while `deferred_`.
    std::string_view sql_{};
    std::string_view statement_sql_{};
    std::span<const db_value> params_{};
    std::string* interpolated_{nullptr};
    // The bytes it put on the wire, once sent.
    std::size_t size_{0};
    bool sent_{false};
    // Interpolated when sent, once every response before it has been read.
    bool deferred_{false};
    // Sent once every response before it has been read, and followed by
    // nothing until its own response has been read.
    bool exclusive_{false};
    // May run a stored procedure (mariadb_statement_classification::calls_routine_).
    bool routine_{false};
    // Its use was counted in the statement cache: when the reader comes back
    // to it, it goes out as text.
    bool counted_{false};
    // Whether `params_` contains a backslash preceded by a non-ASCII byte.
    bool ambiguous_backslash_{false};
    // The session's client character set, read for a deferred statement
    // whose escape depends on it.
    const ma_charset_info_st* charset_{nullptr};
};

// The operation a group's connection runs, one at a time.
enum class mariadb_operation : std::uint8_t {
    none,
    send,
    read,
    store,
    next_result,
    // Reading the session's client character set for the head statement:
    // the query, then its row.
    charset_query,
    charset_store,
    // A statement running alone in the group as a prepared statement.
    prepared,
};

// The pipeline hooks of MariaDB (see db_pipeline_impl.h).
struct mariadb_pipeline_io final {
    using driver_type = mariadb_driver;
    using pool_type = db_pool<mariadb_driver>;
    using state_type = db_pipeline_state<mariadb_pipeline_io>;
    using member_type = db_pipeline_member<mariadb_pipeline_io>;
    using flight_type = mariadb_flight;

    struct group_state final {
        group_state()
            : send_buffer_() {}

        // Frees a stored result nobody took; called whenever the group's
        // flights clear, which every group does before its slot goes away.
        void reset() noexcept;

        // The operation in progress: the wait mask it suspended with, and,
        // once exchange() finished it, its result.
        mariadb_operation operation_{mariadb_operation::none};
        int status_{0};
        bool done_{false};
        int result_{0};
        st_mysql_res* stored_{nullptr};
        // Records from the head that were sent, or dropped unsent because
        // their statement was abandoned first.
        std::size_t sent_{0};
        std::size_t bytes_in_flight_{0};
        // Commands were sent since the reader last paused for a turn.
        bool batched_{false};
        // The command being sent, copied (or, deferred, interpolated) so that
        // a statement abandoned while its send is suspended leaves libmariadb
        // nothing dangling. Above the send budget it is given back once sent.
        std::string send_buffer_;
        // What has been read of the head's responses.
        std::exception_ptr head_failure_{};
        std::exception_ptr head_output_failure_{};
        std::uint64_t head_affected_rows_{0};
        std::uint64_t head_insert_id_{0};
        bool head_returned_rows_{false};
        bool head_extra_row_set_{false};
        // The head statement's prepared execution, while operation_ is
        // prepared.
        mariadb_prepared_run prepared_{};
    };

    // The statement as its caller wrote it; queue() interpolates the
    // parameters unless that waits for the session (mariadb_flight).
    struct statement_type final {
        std::string_view sql_;
        std::span<const db_value> params_;
        std::string interpolated_;
        mariadb_statement_classification classification_{};
        bool deferred_{false};
        bool ambiguous_backslash_{false};
    };

    [[nodiscard]] static statement_type prepare(std::string_view sql, std::span<const db_value> params);
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached prepared statements and the pipeline group sharing
// it.
struct mariadb_session final {
    // Cleared by connect(): a new connection holds no server statements.
    mariadb_statement_cache statements_;
    db_pipeline_group<mariadb_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail

struct pg_conn;
struct pg_result;
// PostgreSQL's part of statement pipelining (db_pipeline.h): a group's
// connection runs in libpq pipeline mode, and each statement sends its own
// flight ending in its own Sync, so the server aborts no statement for another
// one's failure. Flights are queued and flushed as they are sent; the reader
// consumes input and reads results whenever libpq holds them.

namespace ruvia::detail {
// Builds the error for a failed libpq call or result. A statement_failed on a
// connection libpq has marked bad is reported as io_error.
[[nodiscard]] db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type code, const pg_result* result = nullptr);

// Parameters are sent in text format. A double declares float8 so the server
// never infers an integer type from an integral value; every other parameter
// is left untyped (OID 0): the SQL decides its type, and compiled SQL casts
// each placeholder from its value category.
struct postgresql_params final {
    static constexpr std::size_t inline_capacity = 8;
    static constexpr std::size_t numeric_slot_size = 32;

    explicit postgresql_params(std::span<const db_value> params);
    postgresql_params(const postgresql_params&) = delete;
    postgresql_params& operator=(const postgresql_params&) = delete;
    postgresql_params(postgresql_params&&) = delete;
    postgresql_params& operator=(postgresql_params&&) = delete;

    [[nodiscard]] int count() const noexcept {
        return static_cast<int>(count_);
    }

    [[nodiscard]] const char* const* values() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_values_.data() : heap_values_.data();
    }

    [[nodiscard]] const unsigned int* types() const noexcept {
        if (count_ == 0) {
            return nullptr;
        }
        return count_ <= inline_capacity ? inline_types_.data() : heap_types_.data();
    }

    [[nodiscard]] std::span<const unsigned int> type_span() const noexcept {
        return {types(), count_};
    }

    std::size_t count_{0};
    std::array<char, inline_capacity * numeric_slot_size> inline_encoded_;
    std::array<const char*, inline_capacity> inline_values_;
    std::array<unsigned int, inline_capacity> inline_types_;
    std::vector<std::array<char, numeric_slot_size>> heap_encoded_;
    std::vector<const char*> heap_values_;
    // libpq's Oid is unsigned int; 0 leaves the type to the server.
    std::vector<unsigned int> heap_types_;
};

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

// Named server-side statements prepared on one PostgreSQL connection, so a
// repeated statement skips the server's parse and analysis
// (db_statement_cache: keyed by SQL and the parameter types it declares,
// prepared on its second use, least recently used closed for room). A name is
// derived from the key's SHA-256: equal names denote equal statements, so a
// name reaching a server session it was not prepared on (a pooler that shares
// server sessions) can never run a different statement. Every statement in
// flight on a pipelined connection shares the cache.
//
// "ruvia_", 32 lowercase hex digits of the key digest, and a NUL.
using postgresql_statement_name = std::array<char, 39>;
using postgresql_statement_cache = db_statement_cache<postgresql_statement_name>;

[[nodiscard]] postgresql_statement_name make_postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types);

template <typename driver_type>
class db_pool;
struct postgresql_driver;

// The commands of one buffered statement flight. A first use runs as the
// unnamed statement. Otherwise a Close of a statement evicted to make room and
// a Close and Parse of a statement new to the connection precede the
// statement. A Sync ends the flight.
struct postgresql_flight final {
    // The cache entry of a named statement.
    std::optional<postgresql_statement_cache::entry_ref> statement_{};
    // Commands queued ahead of the statement.
    std::size_t setup_commands_{0};
    bool parses_{false};
};

// What has been read of one flight's results, up to its Sync.
struct postgresql_flight_results final {
    // Index of the command whose results are being read; each command's
    // results end with a null result, and the Sync result follows the last.
    std::size_t command_{0};
    std::uint64_t affected_rows_{0};
    // The first error of the flight; the server skips the rest up to Sync.
    std::optional<db_error> failure_{};
    // Keeping the rows failed (an allocation in the caller's resource); the
    // flight is still read up to its Sync, so only its statement fails.
    std::exception_ptr output_failure_{};
    // The error is the statement's own rather than its setup's.
    bool statement_failed_{false};
    // The error reports a stale cached statement (26000 / 0A000) ...
    bool stale_{false};
    // ... found while looking it up or revalidating it, before it ran.
    bool stale_before_execution_{false};
    bool returned_rows_{false};
    bool parsed_{false};
};

// The pipeline hooks of PostgreSQL (see db_pipeline_impl.h).
struct postgresql_pipeline_io final {
    using driver_type = postgresql_driver;
    using pool_type = db_pool<postgresql_driver>;
    using state_type = db_pipeline_state<postgresql_pipeline_io>;
    using member_type = db_pipeline_member<postgresql_pipeline_io>;
    using flight_type = postgresql_flight;

    struct group_state final {
        postgresql_flight_results head_results_{};
        // libpq holds output the socket has not accepted yet.
        bool flush_pending_{false};

        void reset() noexcept {
            head_results_ = {};
            flush_pending_ = false;
        }
    };

    // The statement text, kept by its caller, and its encoded parameters.
    struct statement_type final {
        statement_type(std::string_view sql, std::span<const db_value> params)
            : sql_(sql),
              encoded_(params) {}

        std::string_view sql_;
        postgresql_params encoded_;
    };

    [[nodiscard]] static statement_type prepare(std::string_view sql, std::span<const db_value> params);
    // The connection's socket for a wait; throws when it has none.
    [[nodiscard]] static db_slot_socket::native_socket_type socket(pool_type& pool, std::size_t slot);
    static void open(state_type& state, std::size_t slot);
    [[nodiscard]] static bool queue(
        state_type& state, std::size_t slot, member_type& member, statement_type& statement, flight_type& flight);
    [[nodiscard]] static bool advance(state_type& state, std::size_t slot, member_type& reader);
    [[nodiscard]] static db_socket_readiness wanted(pool_type& pool, std::size_t slot) noexcept;
    static void exchange(state_type& state, std::size_t slot, db_socket_readiness ready);
    [[nodiscard]] static bool reusable(state_type& state, std::size_t slot) noexcept;
};

// Per connection: cached statements and the pipeline group sharing it.
struct postgresql_session final {
    // Cleared by connect(): a new connection holds no server statements.
    postgresql_statement_cache statements_;
    db_pipeline_group<postgresql_pipeline_io> pipeline_;
};

}  // namespace ruvia::detail
