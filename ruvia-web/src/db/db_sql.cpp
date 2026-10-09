#include "db/db_sql.h"

#include <mysql.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/detail/db/db_sql_scan.h"
#include "ruvia/web/detail/db/db_utils.h"

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

void append_string_literal(st_mysql& connection, std::pmr::string& output, std::string_view value) {
    const auto literal_size_hint = mariadb_string_literal_size_hint(value.size());
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
        throw std::length_error("MariaDB SQL is too large");
    }
    const auto offset = output.size();
    output.resize(offset + literal_size_hint);
    output[offset] = '\'';
    const auto length = mysql_real_escape_string(&connection, output.data() + offset + 1,
        value.empty() ? "" : value.data(), static_cast<unsigned long>(value.size()));
    output[offset + 1 + length] = '\'';
    output.resize(offset + length + 2);
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

void append_value_literal(st_mysql& connection, std::pmr::string& output, const db_value& value) {
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            output.append("NULL");
            break;
        case db_value_type::string:
            append_string_literal(connection, output, db_value_access::text(value));
            break;
        case db_value_type::signed_value:
            append_db_number(output, db_value_access::signed_value(value));
            break;
        case db_value_type::unsigned_value:
            append_db_number(output, db_value_access::unsigned_value(value));
            break;
        case db_value_type::double_value:
            append_db_number(output, db_value_access::get_double_value(value));
            break;
        case db_value_type::bool_value:
            output.push_back(db_value_access::get_bool_value(value) ? '1' : '0');
            break;
    }
}

}  // namespace

void validate_mariadb_sql_length(std::size_t length) {
    if (length > (std::numeric_limits<unsigned long>::max)()) {
        throw std::length_error("MariaDB SQL is too large for the client API");
    }
}

db_error mysql_error(
    const st_mysql& connection, std::string_view operation, db_error::code_type error_code) {
    auto* mutable_connection = const_cast<st_mysql*>(&connection);
    const auto* message = mysql_error(mutable_connection);
    const auto code = mysql_errno(mutable_connection);
    const auto* state_value = mysql_sqlstate(mutable_connection);
    std::pmr::string error(operation, detail::process_resource());
    error.append(" failed");
    if (code != 0) {
        error.append(" [errno=");
        append_db_number(error, static_cast<std::uint64_t>(code));
        error.push_back(']');
    }
    if (state_value != nullptr && state_value[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state_value);
        error.push_back(']');
    }
    if (message != nullptr && message[0] != '\0') {
        error.append(": ");
        error.append(message);
    }
    return db_error(error_code, db_driver::mariadb, std::string(error),
        code == 0 ? std::nullopt : std::optional<std::int64_t>(code),
        state_value == nullptr ? std::string{} : std::string(state_value));
}

void free_stored_result(void* result_value) noexcept {
    mysql_free_result(static_cast<st_mysql_res*>(result_value));
}

std::pmr::string interpolate_sql(st_mysql& connection, std::string_view sql,
    std::span<const db_value> params, std::pmr::memory_resource* resource) {
    validate_mariadb_sql_length(sql.size());
    std::pmr::string output(pmr_resource_or_default(resource));
    std::size_t size_hint = sql.size();
    for (const auto& param : params) {
        const auto literal_size_hint = value_literal_size_hint(param);
        if (size_hint > (std::numeric_limits<std::size_t>::max)() - literal_size_hint) {
            throw std::length_error("MariaDB SQL size calculation overflowed");
        }
        size_hint += literal_size_hint;
        validate_mariadb_sql_length(size_hint);
    }
    output.reserve(size_hint);

    // Only a '?' at statement level is a placeholder. One inside a literal, a
    // quoted identifier or a comment is data the statement wants to keep, and
    // substituting it there would push every later parameter one slot along --
    // valid SQL that reads and writes the wrong rows.
    std::size_t offset = 0;
    for (const auto& param : params) {
        const auto placeholder = find_sql_syntax_byte(sql, '?', offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        output.append(sql.data() + offset, placeholder - offset);
        append_value_literal(connection, output, param);
        offset = placeholder + 1;
    }

    if (find_sql_syntax_byte(sql, '?', offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }

    output.append(sql.data() + offset, sql.size() - offset);
    validate_mariadb_sql_length(output.size());
    return output;
}

}  // namespace ruvia::detail
