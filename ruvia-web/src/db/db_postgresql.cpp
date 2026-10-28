#include "db/db_postgresql.h"

#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <openssl/sha.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {
namespace {

// pg_type OID of float8 (double precision); libpq's headers name no type OIDs.
constexpr unsigned int postgresql_float8_oid = 701;

}  // namespace

// The digest covers the SQL, a NUL that SQL never contains, and every type
// OID as four big-endian bytes, so no two keys share an encoding.
postgresql_statement_name make_postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types, std::pmr::memory_resource* resource) {
    const std::size_t key_size = sql.size() + 1U + types.size() * 4U;
    constexpr std::size_t stack_capacity = 512;
    std::array<unsigned char, stack_capacity> stack_key{};
    std::pmr::string heap_key(resource);
    unsigned char* buffer = stack_key.data();
    if (key_size > stack_capacity) {
        heap_key.resize(key_size);
        buffer = reinterpret_cast<unsigned char*>(heap_key.data());
    }
    if (!sql.empty()) {
        std::memcpy(buffer, sql.data(), sql.size());
    }
    std::size_t offset = sql.size();
    buffer[offset++] = 0;
    for (const auto type : types) {
        buffer[offset++] = static_cast<unsigned char>((type >> 24) & 0xffU);
        buffer[offset++] = static_cast<unsigned char>((type >> 16) & 0xffU);
        buffer[offset++] = static_cast<unsigned char>((type >> 8) & 0xffU);
        buffer[offset++] = static_cast<unsigned char>(type & 0xffU);
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(buffer, key_size, digest.data()) == nullptr) {
        throw std::runtime_error("PostgreSQL statement name digest failed");
    }
    constexpr std::string_view prefix{"ruvia_"};
    postgresql_statement_name name{};
    prefix.copy(name.data(), prefix.size());
    // Zero initialization leaves the terminating NUL after the hex digits.
    http_encode_lower_hex(name.data() + prefix.size(), std::span(digest.data(), (name.size() - prefix.size()) / 2));
    return name;
}

db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type error_code, const pg_result* result_value) {
    // libpq marks the connection bad when the transport fails, and reports
    // that failure as a fatal result; it is an I/O failure, not a statement
    // the server rejected.
    if (error_code == db_error::code_type::statement_failed && PQstatus(&connection) == CONNECTION_BAD) {
        error_code = db_error::code_type::io_error;
    }
    const char* state_value = result_value == nullptr
                                  ? nullptr
                                  : PQresultErrorField(const_cast<PGresult*>(result_value), PG_DIAG_SQLSTATE);
    const char* constraint = result_value == nullptr ? nullptr
                                                     : PQresultErrorField(const_cast<PGresult*>(result_value),
                                                           PG_DIAG_CONSTRAINT_NAME);
    const std::string_view state_view =
        (state_value != nullptr && state_value[0] != '\0') ? std::string_view(state_value) : std::string_view{};
    const char* message = result_value == nullptr ? PQerrorMessage(const_cast<PGconn*>(&connection))
                                                  : PQresultErrorMessage(const_cast<PGresult*>(result_value));
    std::string_view message_view{};
    if (message != nullptr && message[0] != '\0') {
        while (*message == ' ' || *message == '\r' || *message == '\n') {
            ++message;
        }
        message_view = message;
        while (!message_view.empty() && (message_view.back() == '\r' || message_view.back() == '\n')) {
            message_view.remove_suffix(1);
        }
    }
    std::string error;
    error.reserve(operation.size() + 7U + (state_view.empty() ? 0U : 12U + state_view.size()) +
                  (message_view.empty() ? 0U : 2U + message_view.size()));
    error.append(operation);
    error.append(" failed");
    if (!state_view.empty()) {
        error.append(" [sqlstate=");
        error.append(state_view);
        error.push_back(']');
    }
    if (!message_view.empty()) {
        error.append(": ");
        error.append(message_view);
    }
    return db_error(error_code, db_driver::postgresql, error, std::nullopt, state_view,
        constraint == nullptr ? std::string_view{} : std::string_view(constraint));
}

postgresql_params::postgresql_params(std::pmr::memory_resource* resource)
    : encoded_(pmr_resource_or_default(resource)),
      values_(pmr_resource_or_default(resource)),
      types_(pmr_resource_or_default(resource)) {}

postgresql_params encode_postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource) {
    auto* resolved = pmr_resource_or_default(resource);
    postgresql_params output(resolved);
    if (params.empty()) {
        return output;
    }
    std::size_t numeric_count = 0;
    for (const auto& param : params) {
        const auto type = db_value_access::type(param);
        if (type == db_value_type::string) {
            const auto& value = db_value_access::owned_text(param);
            if (value.find('\0') != std::string_view::npos) {
                throw std::invalid_argument("PostgreSQL string parameter must not contain NUL bytes");
            }
            if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::length_error("PostgreSQL parameter is too large");
            }
        } else if (type == db_value_type::signed_value || type == db_value_type::unsigned_value ||
                   type == db_value_type::double_value) {
            ++numeric_count;
        }
    }
    output.values_.reserve(params.size());
    output.types_.reserve(params.size());
    if (numeric_count != 0) {
        output.encoded_.reserve(numeric_count);
    }

    for (const auto& param : params) {
        const auto type = db_value_access::type(param);
        output.types_.push_back(type == db_value_type::double_value ? postgresql_float8_oid : 0U);
        const char* text = nullptr;
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string: {
                const auto& value = db_value_access::owned_text(param);
                text = value.c_str();
                break;
            }
            case db_value_type::bool_value:
                text = db_value_access::get_bool_value(param) ? "true" : "false";
                break;
            case db_value_type::signed_value:
            case db_value_type::unsigned_value:
            case db_value_type::double_value: {
                auto& value = output.encoded_.emplace_back();
                if (type == db_value_type::signed_value) {
                    append_db_number(value, db_value_access::signed_value(param));
                } else if (type == db_value_type::unsigned_value) {
                    append_db_number(value, db_value_access::unsigned_value(param));
                } else {
                    append_db_number(value, db_value_access::get_double_value(param));
                }
                text = value.c_str();
                break;
            }
        }
        output.values_.push_back(text);
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
