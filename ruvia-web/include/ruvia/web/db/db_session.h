#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/fixed_string.h"

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
struct request_io_metrics;
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

}  // namespace ruvia
