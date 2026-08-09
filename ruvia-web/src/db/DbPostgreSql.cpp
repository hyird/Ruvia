#include "ruvia/web/detail/db/DbPostgreSql.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {

std::runtime_error postgreSqlError(const pg_conn& connection, std::string_view operation, const pg_result* result) {
    std::pmr::string error(operation, processResource());
    error.append(" failed");
    const char* state = result == nullptr ? nullptr : PQresultErrorField(const_cast<PGresult*>(result), PG_DIAG_SQLSTATE);
    if (state != nullptr && state[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state);
        error.push_back(']');
    }
    const char* message = result == nullptr ? PQerrorMessage(const_cast<PGconn*>(&connection)) : PQresultErrorMessage(const_cast<PGresult*>(result));
    if (message != nullptr && message[0] != '\0') {
        while (*message == ' ' || *message == '\r' || *message == '\n') {
            ++message;
        }
        error.append(": ");
        error.append(message);
        while (!error.empty() && (error.back() == '\r' || error.back() == '\n')) {
            error.pop_back();
        }
    }
    return std::runtime_error(error.c_str());
}

PostgreSqlParams::PostgreSqlParams(std::pmr::memory_resource* resource)
    : encoded(pmrResourceOrDefault(resource)),
      values(pmrResourceOrDefault(resource)),
      lengths(pmrResourceOrDefault(resource)) {}

PostgreSqlParams encodePostgreSqlParams(std::span<const DbValue> params, std::pmr::memory_resource* resource) {
    auto* resolved = pmrResourceOrDefault(resource);
    PostgreSqlParams output(resolved);
    output.encoded.reserve(params.size());
    output.values.reserve(params.size());
    output.lengths.reserve(params.size());

    for (const auto& param : params) {
        output.encoded.emplace_back();
        auto& value = output.encoded.back();
        switch (DbValueAccess::type(param)) {
            case DbValueType::kNull:
                output.values.push_back(nullptr);
                output.lengths.push_back(0);
                continue;
            case DbValueType::kString:
                if (DbValueAccess::text(param).find('\0') != std::string_view::npos) {
                    throw std::invalid_argument("PostgreSQL string parameter must not contain NUL bytes");
                }
                value.assign(DbValueAccess::text(param));
                break;
            case DbValueType::kSigned:
                appendDbNumber(value, DbValueAccess::signedValue(param));
                break;
            case DbValueType::kUnsigned:
                appendDbNumber(value, DbValueAccess::unsignedValue(param));
                break;
            case DbValueType::kDouble:
                appendDbNumber(value, DbValueAccess::doubleValue(param));
                break;
            case DbValueType::kBool:
                value.assign(DbValueAccess::boolValue(param) ? "true" : "false");
                break;
        }
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::length_error("PostgreSQL parameter is too large");
        }
        output.values.push_back(value.c_str());
        output.lengths.push_back(static_cast<int>(value.size()));
    }
    return output;
}

std::uint64_t postgreSqlAffectedRows(const pg_result& result) noexcept {
    const auto* text = PQcmdTuples(const_cast<PGresult*>(&result));
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    std::uint64_t value = 0;
    const auto* end = text;
    while (*end >= '0' && *end <= '9') {
        ++end;
    }
    const auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end ? value : 0;
}

}  // namespace ruvia::detail
