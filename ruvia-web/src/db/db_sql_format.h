#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_schema.h"
#include "ruvia/web/db/db_types.h"

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
