#include "db/db_sql.h"

#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/web/detail/db/db_sql_scan.h"
#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_sql_format.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t mariadb_string_literal_size_hint(std::size_t value_size) {
    // mysql_real_escape_string may expand every input byte to two output
    // bytes, and its input/output lengths are both unsigned long. Check both
    // bounds before doing the multiplication or passing the input to C.
    if (value_size > (std::numeric_limits<unsigned long>::max)() / 2 ||
        value_size > ((std::numeric_limits<std::size_t>::max)() - 2) / 2) {
        throw std::length_error("MariaDB string parameter is too large");
    }
    return value_size * 2 + 2;
}

// A backslash in `value` that a backslash put in front of it could join to
// the byte before: one that follows a byte above 0x7F, which a multibyte
// character set may read as a character's lead byte.
[[nodiscard]] bool ambiguous_backslash(std::string_view value) noexcept {
    for (auto backslash = value.find('\\'); backslash != std::string_view::npos;
        backslash = value.find('\\', backslash + 1)) {
        if (backslash != 0 && static_cast<unsigned char>(value[backslash - 1]) > 0x7f) {
            return true;
        }
    }
    return false;
}

// A string literal: each quote doubled, which reads the same in every
// sql_mode and character set, as no character set has a multibyte character
// ending in a quote byte; and, when the session reads backslash escapes, each
// backslash doubled. `charset` keeps a multibyte character whole, so that a
// backslash byte inside one is not escaped; without it, no backslash may
// follow a byte above 0x7F (ambiguous_backslash()), and an ASCII byte never
// joins the backslash put after it.
void append_string_literal(std::pmr::string& output, std::string_view value, bool backslash_escapes,
    const MARIADB_CHARSET_INFO* charset) {
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("MariaDB string parameter must not contain NUL bytes");
    }
    const auto literal_size_hint = mariadb_string_literal_size_hint(value.size());
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
        throw std::length_error("MariaDB SQL is too large");
    }
    const bool escape_backslashes = backslash_escapes && value.find('\\') != std::string_view::npos;
    const bool multibyte = escape_backslashes && charset != nullptr && charset->char_maxlen > 1;
    if (escape_backslashes && charset == nullptr && ambiguous_backslash(value)) {
        throw std::logic_error("MariaDB string parameter needs the session's character set to be escaped");
    }
    output.push_back('\'');
    if (!multibyte) {
        const auto special = escape_backslashes ? std::string_view("'\\", 2) : std::string_view("'", 1);
        for (auto at = value.find_first_of(special); at != std::string_view::npos; at = value.find_first_of(special)) {
            output.append(value.substr(0, at + 1));
            output.push_back(value[at]);
            value.remove_prefix(at + 1);
        }
        output.append(value);
    } else {
        const char* const end = value.data() + value.size();
        for (const char* cursor = value.data(); cursor != end;) {
            if (const auto length = charset->mb_valid(cursor, end); length > 1) {
                output.append(cursor, length);
                cursor += length;
                continue;
            }
            const char byte = *cursor++;
            if (byte == '\'' || byte == '\\') {
                output.push_back(byte);
            }
            output.push_back(byte);
        }
    }
    output.push_back('\'');
}

[[nodiscard]] std::size_t value_literal_size_hint(const db_value& value) {
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            return 4;
        case db_value_type::string:
            return mariadb_string_literal_size_hint(db_value_access::text(value).size());
        case db_value_type::signed_value:
        case db_value_type::unsigned_value:
            return 32;
        case db_value_type::double_value:
            return 64;
        case db_value_type::bool_value:
            return 1;
    }
    return 0;
}

void append_value_literal(
    std::pmr::string& output, const db_value& value, bool backslash_escapes, const MARIADB_CHARSET_INFO* charset) {
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            output.append("NULL");
            break;
        case db_value_type::string:
            append_string_literal(output, db_value_access::text(value), backslash_escapes, charset);
            break;
        case db_value_type::signed_value:
            append_db_number(output, db_value_access::signed_value(value));
            break;
        case db_value_type::unsigned_value:
            append_db_number(output, db_value_access::unsigned_value(value));
            break;
        case db_value_type::double_value:
            append_db_double_literal(output, db_value_access::get_double_value(value), db_driver::mariadb);
            break;
        case db_value_type::bool_value:
            output.push_back(db_value_access::get_bool_value(value) ? '1' : '0');
            break;
    }
}

// A server error after which the server ends the session, or which shows
// that it read a packet it did not expect.
[[nodiscard]] bool session_ended(unsigned int native_code) noexcept {
    switch (native_code) {
        case ER_SERVER_SHUTDOWN:
        case ER_FORCING_CLOSE:
        case ER_ABORTING_CONNECTION:
        case ER_NET_PACKET_TOO_LARGE:
        case ER_NET_READ_ERROR_FROM_PIPE:
        case ER_NET_FCNTL_ERROR:
        case ER_NET_PACKETS_OUT_OF_ORDER:
        case ER_NET_UNCOMPRESS_ERROR:
        case ER_NET_READ_ERROR:
        case ER_NET_READ_INTERRUPTED:
        case ER_NET_ERROR_ON_WRITE:
        case ER_NET_WRITE_INTERRUPTED:
        case ER_NEW_ABORTING_CONNECTION:
        case ER_CONNECTION_KILLED:
            return true;
        default:
            return false;
    }
}

// One statement-level token of MariaDB SQL.
struct sql_token final {
    enum class kind : std::uint8_t {
        end,
        word,
        number,
        placeholder,
        comma,
        open,
        // An executable comment ("/*!" or "/*M!"): the server runs its
        // content as SQL.
        hidden,
        other,
    };

    kind kind_{kind::end};
    std::string_view text_{};
};

[[nodiscard]] constexpr bool sql_word_byte(char character) noexcept {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '$' || byte >= 0x80;
}

[[nodiscard]] constexpr bool sql_keyword(std::string_view word, std::string_view keyword) noexcept {
    if (word.size() != keyword.size()) {
        return false;
    }
    for (std::size_t index = 0; index < word.size(); ++index) {
        auto byte = word[index];
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<char>(byte - 'A' + 'a');
        }
        if (byte != keyword[index]) {
            return false;
        }
    }
    return true;
}

// Reads the token at `index` and moves past it. Whitespace and comments only
// separate tokens; a literal or quoted identifier is one `other` token.
[[nodiscard]] sql_token next_sql_token(std::string_view sql, std::size_t& index, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    while (index < sql.size()) {
        const auto start = index;
        const char character = sql[index];
        if (is_sql_whitespace(character)) {
            ++index;
            continue;
        }
        const bool block_comment = character == '/' && sql.substr(index, 2) == "/*";
        if (block_comment || character == '#' || is_mariadb_double_dash_comment(sql, index)) {
            index = skip_sql_atom(sql, index, backslash_escapes);
            if (block_comment && (sql.substr(start, 3) == "/*!" || sql.substr(start, 4) == "/*M!")) {
                return {kind::hidden, sql.substr(start, index - start)};
            }
            continue;
        }
        if (character == '\'' || character == '"' || character == '`') {
            index = skip_sql_atom(sql, index, backslash_escapes);
            return {kind::other, sql.substr(start, index - start)};
        }
        if (sql_word_byte(character)) {
            while (index < sql.size() && sql_word_byte(sql[index])) {
                ++index;
            }
            const bool number = character >= '0' && character <= '9';
            return {number ? kind::number : kind::word, sql.substr(start, index - start)};
        }
        ++index;
        switch (character) {
            case '?':
                return {kind::placeholder, sql.substr(start, 1)};
            case ',':
                return {kind::comma, sql.substr(start, 1)};
            case '(':
                return {kind::open, sql.substr(start, 1)};
            default:
                return {kind::other, sql.substr(start, 1)};
        }
    }
    return {};
}

// The first token past opening parentheses.
[[nodiscard]] sql_token leading_sql_token(std::string_view sql, bool backslash_escapes) noexcept {
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == sql_token::kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    return token;
}

// A placeholder where the text protocol takes only an integer literal, by the
// tokens before it, most recent first: "LIMIT ?", "OFFSET ?", "FETCH FIRST ?",
// "FETCH NEXT ?", "ROWS EXAMINED ?" and the second count of "LIMIT n, ?".
[[nodiscard]] bool integer_placeholder(const std::array<sql_token, 3>& before) noexcept {
    using kind = sql_token::kind;
    if (before[0].kind_ == kind::word) {
        const auto word = before[0].text_;
        return sql_keyword(word, "limit") || sql_keyword(word, "offset") || sql_keyword(word, "first") ||
               sql_keyword(word, "next") || sql_keyword(word, "examined");
    }
    return before[0].kind_ == kind::comma &&
           (before[1].kind_ == kind::placeholder || before[1].kind_ == kind::number) &&
           before[2].kind_ == kind::word && sql_keyword(before[2].text_, "limit");
}

// The traits of `sql` read in one backslash mode; nullopt-like `placeholders`
// lets the caller compare the two readings.
struct sql_reading final {
    mariadb_statement_traits traits_{true, 0};
    std::size_t placeholders_{0};
};

[[nodiscard]] sql_reading read_mariadb_statement(std::string_view sql, bool backslash_escapes) noexcept {
    using kind = sql_token::kind;
    sql_reading reading;
    auto& traits = reading.traits_;
    std::size_t index = 0;
    auto token = next_sql_token(sql, index, backslash_escapes);
    while (token.kind_ == kind::open) {
        token = next_sql_token(sql, index, backslash_escapes);
    }
    if (token.kind_ == kind::hidden ||
        (token.kind_ == kind::word && (sql_keyword(token.text_, "call") || sql_keyword(token.text_, "load") ||
                                          sql_keyword(token.text_, "set") || sql_keyword(token.text_, "use")))) {
        traits.preparable_ = false;
        return reading;
    }
    std::array<sql_token, 3> before{};
    for (; token.kind_ != kind::end; token = next_sql_token(sql, index, backslash_escapes)) {
        if (token.kind_ == kind::hidden || (token.kind_ == kind::word && sql_keyword(token.text_, "returning"))) {
            traits.preparable_ = false;
            return reading;
        }
        if (token.kind_ == kind::placeholder) {
            if (integer_placeholder(before)) {
                if (reading.placeholders_ >= 64) {
                    traits.preparable_ = false;
                    return reading;
                }
                traits.integer_params_ |= std::uint64_t{1} << reading.placeholders_;
            }
            ++reading.placeholders_;
        }
        before = {token, before[0], before[1]};
    }
    return reading;
}

}  // namespace

void validate_mariadb_sql_length(std::size_t length) {
    if (length > (std::numeric_limits<unsigned long>::max)()) {
        throw std::length_error("MariaDB SQL is too large for the client API");
    }
}

db_error mariadb_error(std::string_view operation, unsigned int native_code, const char* sql_state,
    const char* message, db_error::code_type error_code) {
    // A statement that failed because the connection did is an I/O failure,
    // not a statement the server rejected.
    if (error_code == db_error::code_type::statement_failed &&
        (native_code == CR_SERVER_GONE_ERROR || native_code == CR_SERVER_LOST || native_code == CR_ERR_NET_READ ||
            native_code == CR_ERR_NET_WRITE || session_ended(native_code))) {
        error_code = db_error::code_type::io_error;
    }
    const std::string_view state_view =
        (sql_state != nullptr && sql_state[0] != '\0') ? std::string_view(sql_state) : std::string_view{};
    const std::string_view message_view =
        (message != nullptr && message[0] != '\0') ? std::string_view(message) : std::string_view{};
    std::array<char, http_max_unsigned_decimal_size> digits;
    const std::string_view code_view =
        native_code != 0 ? http_format_unsigned(digits, static_cast<std::uint64_t>(native_code)) : std::string_view{};
    std::string error;
    error.reserve(operation.size() + 7U + (code_view.empty() ? 0U : 9U + code_view.size()) +
                  (state_view.empty() ? 0U : 12U + state_view.size()) +
                  (message_view.empty() ? 0U : 2U + message_view.size()));
    error.append(operation);
    error.append(" failed");
    if (!code_view.empty()) {
        error.append(" [errno=");
        error.append(code_view);
        error.push_back(']');
    }
    if (!state_view.empty()) {
        error.append(" [sqlstate=");
        error.append(state_view);
        error.push_back(']');
    }
    if (!message_view.empty()) {
        error.append(": ");
        error.append(message_view);
    }
    return db_error(error_code, db_driver::mariadb, error,
        native_code == 0 ? std::nullopt : std::optional<std::int64_t>(native_code),
        sql_state == nullptr ? std::string_view{} : std::string_view(sql_state));
}

db_error mysql_error(const st_mysql& connection, std::string_view operation, db_error::code_type error_code) {
    auto* mutable_connection = const_cast<st_mysql*>(&connection);
    return mariadb_error(operation, mysql_errno(mutable_connection), mysql_sqlstate(mutable_connection),
        mysql_error(mutable_connection), error_code);
}

bool mariadb_client_error(unsigned int native_code) noexcept {
    return (native_code >= CR_MIN_ERROR && native_code <= CR_MAX_ERROR) ||
           (native_code >= CER_MIN_ERROR && native_code <= CER_MAX_ERROR);
}

bool mariadb_connection_failure(unsigned int native_code) noexcept {
    // A failure without a code has no known cause.
    return native_code == 0 || mariadb_client_error(native_code) || session_ended(native_code);
}

void free_stored_result(void* result_value) noexcept {
    mysql_free_result(static_cast<st_mysql_res*>(result_value));
}

bool mariadb_interpolation_needs_session(std::string_view sql, std::span<const db_value> params) noexcept {
    if (params.empty()) {
        return false;
    }
    if (sql.find('\\') != std::string_view::npos) {
        return true;
    }
    for (const auto& param : params) {
        if (db_value_access::type(param) == db_value_type::string &&
            db_value_access::text(param).find('\\') != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool mariadb_interpolation_needs_charset(const st_mysql& connection, std::span<const db_value> params) noexcept {
    if ((connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) != 0) {
        return false;
    }
    for (const auto& param : params) {
        if (db_value_access::type(param) == db_value_type::string && ambiguous_backslash(db_value_access::text(param))) {
            return true;
        }
    }
    return false;
}

const MARIADB_CHARSET_INFO& mariadb_queried_charset(MYSQL_RES& result) {
    auto* row = mysql_fetch_row(&result);
    const auto* lengths = row == nullptr ? nullptr : mysql_fetch_lengths(&result);
    if (row == nullptr || row[0] == nullptr || lengths == nullptr || mysql_num_fields(&result) != 1) {
        throw std::invalid_argument("MariaDB did not name the session's character set");
    }
    const std::string name(row[0], lengths[0]);
    const auto* charset = mariadb_get_charset_by_name(name.c_str());
    if (charset == nullptr) {
        throw std::invalid_argument("MariaDB session character set " + name +
                                    " is unknown to the client library, so a string parameter with a backslash "
                                    "cannot be escaped for it");
    }
    return *charset;
}

const MARIADB_CHARSET_INFO& mariadb_utf8mb4_charset() noexcept {
    // libmariadb's table of character sets is static and always has it.
    static const MARIADB_CHARSET_INFO& charset = *mariadb_get_charset_by_name("utf8mb4");
    return charset;
}

void interpolate_sql(st_mysql& connection, std::string_view sql, std::span<const db_value> params,
    std::pmr::string& output, const MARIADB_CHARSET_INFO* charset) {
    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("SQL must not contain NUL bytes");
    }
    validate_mariadb_sql_length(sql.size());
    const bool backslash_escapes = (connection.server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) == 0;
    std::size_t size_hint = sql.size();
    static constexpr std::size_t inline_placeholder_capacity = 32;
    std::array<std::size_t, inline_placeholder_capacity> inline_placeholders{};
    std::size_t scan_offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto placeholder = find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        if (i < inline_placeholder_capacity) {
            inline_placeholders[i] = placeholder;
        }
        scan_offset = placeholder + 1;
        if (db_value_access::type(param) == db_value_type::string) {
            const auto value = db_value_access::text(param);
            if (value.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("MariaDB string parameter must not contain NUL bytes");
            }
            if (backslash_escapes && charset == nullptr &&
                value.find('\\') != std::string_view::npos && ambiguous_backslash(value)) {
                throw std::logic_error(
                    "MariaDB string parameter needs the session's character set to be escaped");
            }
        }
        const auto literal_size_hint = value_literal_size_hint(param);
        if (size_hint > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
            throw std::length_error("MariaDB SQL size calculation overflowed");
        }
        size_hint += literal_size_hint;
        validate_mariadb_sql_length(size_hint);
    }
    if (find_sql_syntax_byte(sql, '?', backslash_escapes, scan_offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }
    output.clear();
    output.reserve(size_hint);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto placeholder = i < inline_placeholder_capacity
                                     ? inline_placeholders[i]
                                     : find_sql_syntax_byte(sql, '?', backslash_escapes, offset);
        output.append(sql.data() + offset, placeholder - offset);
        append_value_literal(output, params[i], backslash_escapes, charset);
        offset = placeholder + 1;
    }
    output.append(sql.data() + offset, sql.size() - offset);
    validate_mariadb_sql_length(output.size());
}

mariadb_statement_traits mariadb_statement_traits_of(std::string_view sql) noexcept {
    // A backslash ends a literal in one mode and escapes in the other; a
    // statement whose two readings disagree on its placeholders is not
    // prepared, and either reading's integer placeholders count.
    const auto escaped = read_mariadb_statement(sql, true);
    if (sql.find('\\') == std::string_view::npos) {
        return escaped.traits_;
    }
    const auto plain = read_mariadb_statement(sql, false);
    if (!escaped.traits_.preparable_ || !plain.traits_.preparable_ || escaped.placeholders_ != plain.placeholders_) {
        return {false, 0};
    }
    return {true, escaped.traits_.integer_params_ | plain.traits_.integer_params_};
}

bool mariadb_exclusive_statement(std::string_view sql) noexcept {
    const auto leading = leading_sql_token(sql, true);
    return leading.kind_ == sql_token::kind::hidden ||
           (leading.kind_ == sql_token::kind::word && sql_keyword(leading.text_, "load"));
}

bool mariadb_calls_routine(std::string_view sql) noexcept {
    const auto leading = leading_sql_token(sql, true);
    return leading.kind_ == sql_token::kind::hidden ||
           (leading.kind_ == sql_token::kind::word && sql_keyword(leading.text_, "call"));
}

}  // namespace ruvia::detail
