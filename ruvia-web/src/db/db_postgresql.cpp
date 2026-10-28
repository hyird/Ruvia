#include "db/db_postgresql.h"

#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <cmath>
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
    std::array<unsigned char, stack_capacity> stack_key;
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
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
    if (SHA256(buffer, key_size, digest.data()) == nullptr) {
        throw std::runtime_error("PostgreSQL statement name digest failed");
    }
    constexpr std::string_view prefix{"ruvia_"};
    postgresql_statement_name name;
    prefix.copy(name.data(), prefix.size());
    http_encode_lower_hex(name.data() + prefix.size(), std::span(digest.data(), (name.size() - prefix.size()) / 2));
    name.back() = '\0';
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

postgresql_params::postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource)
    : count_(params.size()),
      heap_encoded_(pmr_resource_or_default(resource)),
      heap_values_(pmr_resource_or_default(resource)),
      heap_types_(pmr_resource_or_default(resource)) {
    if (params.empty()) {
        return;
    }
    const bool inline_params = params.size() <= inline_capacity;
    std::size_t numeric_count = 0;
    if (!inline_params) {
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
                if (type == db_value_type::double_value && !std::isfinite(db_value_access::get_double_value(param))) {
                    throw std::invalid_argument("database double value must be finite");
                }
                ++numeric_count;
            }
        }
        heap_values_.reserve(params.size());
        heap_types_.reserve(params.size());
    }
    const bool inline_numerics = inline_params || numeric_count <= inline_capacity;
    if (!inline_numerics) {
        heap_encoded_.resize(numeric_count);
    }

    std::size_t numeric_index = 0;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto type = db_value_access::type(param);
        const unsigned int oid = type == db_value_type::double_value ? postgresql_float8_oid : 0U;
        const char* text = nullptr;
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string: {
                const auto& value = db_value_access::owned_text(param);
                if (inline_params) {
                    if (value.find('\0') != std::string_view::npos) {
                        throw std::invalid_argument("PostgreSQL string parameter must not contain NUL bytes");
                    }
                    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                        throw std::length_error("PostgreSQL parameter is too large");
                    }
                }
                text = value.c_str();
                break;
            }
            case db_value_type::bool_value:
                text = db_value_access::get_bool_value(param) ? "true" : "false";
                break;
            case db_value_type::signed_value:
            case db_value_type::unsigned_value:
            case db_value_type::double_value: {
                if (inline_params && type == db_value_type::double_value &&
                    !std::isfinite(db_value_access::get_double_value(param))) {
                    throw std::invalid_argument("database double value must be finite");
                }
                char* const slot = inline_numerics
                                       ? inline_encoded_.data() + numeric_index * numeric_slot_size
                                       : heap_encoded_[numeric_index].data();
                ++numeric_index;
                std::to_chars_result formatted{};
                if (type == db_value_type::signed_value) {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, db_value_access::signed_value(param));
                } else if (type == db_value_type::unsigned_value) {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, db_value_access::unsigned_value(param));
                } else {
                    formatted = std::to_chars(slot, slot + numeric_slot_size - 1, db_value_access::get_double_value(param));
                }
                if (formatted.ec != std::errc{}) {
                    throw std::logic_error("failed to format PostgreSQL numeric parameter");
                }
                *formatted.ptr = '\0';
                text = slot;
                break;
            }
        }
        if (inline_params) {
            inline_types_[i] = oid;
            inline_values_[i] = text;
        } else {
            heap_types_.push_back(oid);
            heap_values_.push_back(text);
        }
    }
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
