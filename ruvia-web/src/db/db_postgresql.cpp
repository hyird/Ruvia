#include "db/db_postgresql.h"

#include <libpq-fe.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/detail/db/db_utils.h"

namespace ruvia::detail {

db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type error_code, const pg_result* result_value) {
    std::pmr::string error(operation, process_resource());
    error.append(" failed");
    const char* state_value = result_value == nullptr
                                  ? nullptr
                                  : PQresultErrorField(const_cast<PGresult*>(result_value), PG_DIAG_SQLSTATE);
    const char* constraint = result_value == nullptr ? nullptr
                                                     : PQresultErrorField(const_cast<PGresult*>(result_value),
                                                           PG_DIAG_CONSTRAINT_NAME);
    if (state_value != nullptr && state_value[0] != '\0') {
        error.append(" [sqlstate=");
        error.append(state_value);
        error.push_back(']');
    }
    const char* message = result_value == nullptr ? PQerrorMessage(const_cast<PGconn*>(&connection))
                                                  : PQresultErrorMessage(const_cast<PGresult*>(result_value));
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
    return db_error(error_code, db_driver::postgresql, std::string(error), std::nullopt,
        state_value == nullptr ? std::string{} : std::string(state_value),
        constraint == nullptr ? std::string{} : std::string(constraint));
}

postgresql_params::postgresql_params(std::pmr::memory_resource* resource)
    : encoded_(pmr_resource_or_default(resource)),
      values_(pmr_resource_or_default(resource)),
      lengths_(pmr_resource_or_default(resource)) {}

postgresql_params encode_postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource) {
    auto* resolved = pmr_resource_or_default(resource);
    postgresql_params output(resolved);
    output.encoded_.reserve(params.size());
    output.values_.reserve(params.size());
    output.lengths_.reserve(params.size());

    for (const auto& param : params) {
        output.encoded_.emplace_back();
        auto& value = output.encoded_.back();
        switch (db_value_access::type(param)) {
            case db_value_type::null:
                output.values_.push_back(nullptr);
                output.lengths_.push_back(0);
                continue;
            case db_value_type::string:
                if ((db_value_access::text(param).find('\0') != std::string_view::npos)) {
                    throw std::invalid_argument(
                        "PostgreSQL string parameter must not contain NUL bytes");
                }
                value.assign(db_value_access::text(param));
                break;
            case db_value_type::signed_value:
                append_db_number(value, db_value_access::signed_value(param));
                break;
            case db_value_type::unsigned_value:
                append_db_number(value, db_value_access::unsigned_value(param));
                break;
            case db_value_type::double_value:
                append_db_number(value, db_value_access::get_double_value(param));
                break;
            case db_value_type::bool_value:
                value.assign(db_value_access::get_bool_value(param) ? "true" : "false");
                break;
        }
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::length_error("PostgreSQL parameter is too large");
        }
        output.values_.push_back(value.c_str());
        output.lengths_.push_back(static_cast<int>(value.size()));
    }
    return output;
}

std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept {
    const auto* text = PQcmdTuples(const_cast<PGresult*>(&result_value));
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    std::uint64_t value = 0;
    const auto* end = text;
    while (*end >= '0' && *end <= '9') {
        ++end;
    }
    const auto parsed_value = std::from_chars(text, end, value);
    return parsed_value.ec == std::errc{} && parsed_value.ptr == end ? value : 0;
}

}  // namespace ruvia::detail
