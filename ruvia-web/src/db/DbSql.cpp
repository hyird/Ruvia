#include "ruvia/web/detail/db/DbSql.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/db/DbSqlScan.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t mariaDbStringLiteralSizeHint(std::size_t valueSize) {
    // mysql_real_escape_string may expand every input byte to two output
    // bytes, and its input/output lengths are both unsigned long. Check both
    // bounds before doing the multiplication or passing the input to C.
    if (valueSize > (std::numeric_limits<unsigned long>::max)() / 2 || valueSize > ((std::numeric_limits<std::size_t>::max)() - 2) / 2) {
        throw std::length_error("MariaDB string parameter is too large");
    }
    return valueSize * 2 + 2;
}

void appendStringLiteral(st_mysql& connection, std::pmr::string& output, std::string_view value) {
    const auto literalSizeHint = mariaDbStringLiteralSizeHint(value.size());
    if (output.size() > (std::numeric_limits<std::size_t>::max)() - literalSizeHint) {
        throw std::length_error("MariaDB SQL is too large");
    }
    output.push_back('\'');
    const auto offset = output.size();
    output.resize(offset + literalSizeHint - 1);
    const auto length = mysql_real_escape_string(&connection, output.data() + offset, value.empty() ? "" : value.data(), static_cast<unsigned long>(value.size()));
    output.resize(offset + length);
    output.push_back('\'');
}

[[nodiscard]] std::size_t valueLiteralSizeHint(const DbValue& value) {
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            return 4;
        case DbValueType::kString:
            return mariaDbStringLiteralSizeHint(DbValueAccess::text(value).size());
        case DbValueType::kSigned:
        case DbValueType::kUnsigned:
            return 32;
        case DbValueType::kDouble:
            return 64;
        case DbValueType::kBool:
            return 1;
    }
    return 0;
}

void appendValueLiteral(st_mysql& connection, std::pmr::string& output, const DbValue& value) {
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            output.append("NULL");
            break;
        case DbValueType::kString:
            appendStringLiteral(connection, output, DbValueAccess::text(value));
            break;
        case DbValueType::kSigned:
            appendDbNumber(output, DbValueAccess::signedValue(value));
            break;
        case DbValueType::kUnsigned:
            appendDbNumber(output, DbValueAccess::unsignedValue(value));
            break;
        case DbValueType::kDouble:
            appendDbNumber(output, DbValueAccess::doubleValue(value));
            break;
        case DbValueType::kBool:
            output.push_back(DbValueAccess::boolValue(value) ? '1' : '0');
            break;
    }
}

}  // namespace

void validateMariaDbSqlLength(std::size_t length) {
    if (length > (std::numeric_limits<unsigned long>::max)()) {
        throw std::length_error("MariaDB SQL is too large for the client API");
    }
}

std::runtime_error mysqlError(const st_mysql& connection, std::string_view operation) {
    auto* mutableConnection = const_cast<st_mysql*>(&connection);
    const auto* message = mysql_error(mutableConnection);
    const auto code = mysql_errno(mutableConnection);
    const auto* state = mysql_sqlstate(mutableConnection);
    std::pmr::string error(operation, detail::processResource());
    error.append(" failed");
    if (code != 0) {
        error.append(" [errno=");
        appendDbNumber(error, static_cast<std::uint64_t>(code));
        error.push_back(']');
    }
    if (state != nullptr && state[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state);
        error.push_back(']');
    }
    if (message != nullptr && message[0] != '\0') {
        error.append(": ");
        error.append(message);
    }
    return std::runtime_error(error.c_str());
}

void freeStoredResult(void* result) noexcept {
    mysql_free_result(static_cast<st_mysql_res*>(result));
}

std::pmr::string interpolateSql(st_mysql& connection, std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource) {
    validateMariaDbSqlLength(sql.size());
    std::pmr::string output(pmrResourceOrDefault(resource));
    std::size_t sizeHint = sql.size();
    for (const auto& param : params) {
        const auto literalSizeHint = valueLiteralSizeHint(param);
        if (sizeHint > (std::numeric_limits<std::size_t>::max)() - literalSizeHint) {
            throw std::length_error("MariaDB SQL size calculation overflowed");
        }
        sizeHint += literalSizeHint;
        validateMariaDbSqlLength(sizeHint);
    }
    output.reserve(sizeHint);

    // Only a '?' at statement level is a placeholder. One inside a literal, a
    // quoted identifier or a comment is data the statement wants to keep, and
    // substituting it there would push every later parameter one slot along --
    // valid SQL that reads and writes the wrong rows.
    std::size_t offset = 0;
    for (const auto& param : params) {
        const auto placeholder = findSqlSyntaxByte(sql, '?', offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        output.append(sql.data() + offset, placeholder - offset);
        appendValueLiteral(connection, output, param);
        offset = placeholder + 1;
    }

    if (findSqlSyntaxByte(sql, '?', offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }

    output.append(sql.data() + offset, sql.size() - offset);
    validateMariaDbSqlLength(output.size());
    return output;
}

}  // namespace ruvia::detail
