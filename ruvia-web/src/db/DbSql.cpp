#include "ruvia/web/detail/db/DbSql.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <mysql/mysql.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>

namespace ruvia::detail {
namespace {

void appendStringLiteral(st_mysql& connection, std::pmr::string& output, std::string_view value) {
    output.push_back('\'');
    const auto offset = output.size();
    output.resize_and_overwrite(
        offset + value.size() * 2 + 1,
        [&connection, value, offset](char* data, std::size_t) noexcept {
            const auto length = mysql_real_escape_string(
                &connection,
                data + offset,
                value.empty() ? "" : value.data(),
                static_cast<unsigned long>(value.size()));
            return offset + length;
        });
    output.push_back('\'');
}

[[nodiscard]] std::size_t valueLiteralSizeHint(const DbValue& value) noexcept {
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            return 4;
        case DbValueType::kString:
            return DbValueAccess::text(value).size() * 2 + 2;
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
            appendStringLiteral(
                connection, output, DbValueAccess::text(value));
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

std::pmr::string interpolateSql(
    st_mysql& connection,
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(pmrResourceOrDefault(resource));
    std::size_t sizeHint = sql.size();
    for (const auto& param : params) {
        sizeHint += valueLiteralSizeHint(param);
    }
    output.reserve(sizeHint);

    std::size_t offset = 0;
    for (const auto& param : params) {
        const auto placeholder = sql.find('?', offset);
        if (placeholder == std::string_view::npos) {
            throw std::invalid_argument("SQL parameter count does not match placeholders");
        }
        output.append(sql.data() + offset, placeholder - offset);
        appendValueLiteral(connection, output, param);
        offset = placeholder + 1;
    }

    if (sql.find('?', offset) != std::string_view::npos) {
        throw std::invalid_argument("SQL parameter count does not match placeholders");
    }

    output.append(sql.data() + offset, sql.size() - offset);
    return output;
}

}  // namespace ruvia::detail
