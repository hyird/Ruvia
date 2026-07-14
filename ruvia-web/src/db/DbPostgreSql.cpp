#include "ruvia/web/detail/db/DbPostgreSql.h"

#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <charconv>
#include <cstdint>
#include <limits>

namespace ruvia::detail {

std::runtime_error postgreSqlError(
    const pg_conn& connection,
    std::string_view operation,
    const pg_result* result) {
    std::pmr::string error(operation, processResource());
    error.append(" failed");
    const char* state = result == nullptr
        ? nullptr
        : PQresultErrorField(const_cast<PGresult*>(result), PG_DIAG_SQLSTATE);
    if (state != nullptr && state[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state);
        error.push_back(']');
    }
    const char* message = result == nullptr
        ? PQerrorMessage(const_cast<PGconn*>(&connection))
        : PQresultErrorMessage(const_cast<PGresult*>(result));
    if (message != nullptr && message[0] != '\0') {
        while (*message == ' ' || *message == '\r' || *message == '\n') {
            ++message;
        }
        error.append(": ");
        error.append(message);
        while (!error.empty() &&
               (error.back() == '\r' || error.back() == '\n')) {
            error.pop_back();
        }
    }
    return std::runtime_error(error.c_str());
}

PostgreSqlParams::PostgreSqlParams(std::pmr::memory_resource* resource)
    : encoded(pmrResourceOrDefault(resource)),
      values(pmrResourceOrDefault(resource)) {}

PostgreSqlParams encodePostgreSqlParams(
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    auto* resolved = pmrResourceOrDefault(resource);
    PostgreSqlParams output(resolved);
    output.encoded.reserve(params.size());
    output.values.reserve(params.size());

    for (const auto& param : params) {
        output.encoded.emplace_back();
        auto& value = output.encoded.back();
        switch (param.type()) {
            case DbValueType::kNull:
                output.values.push_back(nullptr);
                continue;
            case DbValueType::kString:
                value.assign(param.text());
                break;
            case DbValueType::kSigned:
                appendDbNumber(value, param.signedValue());
                break;
            case DbValueType::kUnsigned:
                appendDbNumber(value, param.unsignedValue());
                break;
            case DbValueType::kDouble:
                appendDbNumber(value, param.doubleValue());
                break;
            case DbValueType::kBool:
                value.assign(param.boolValue() ? "true" : "false");
                break;
        }
        output.values.push_back(value.c_str());
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
