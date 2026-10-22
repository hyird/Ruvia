#include "db/db_postgresql.h"

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <openssl/sha.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/web/detail/db/db_utils.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {
namespace {

// pg_type OID of float8 (double precision); libpq's headers name no type OIDs.
constexpr unsigned int postgresql_float8_oid = 701;

[[nodiscard]] std::size_t postgresql_statement_hash(std::string_view sql, std::span<const unsigned int> types) noexcept {
    auto hash = std::hash<std::string_view>{}(sql) ^ types.size();
    for (const auto type : types) {
        hash = (hash ^ type) * 0x100000001b3ULL;
    }
    return hash;
}

// The digest covers the SQL, a NUL that SQL never contains, and every type
// OID as four big-endian bytes, so no two keys share an encoding.
[[nodiscard]] postgresql_statement_cache::name_type postgresql_statement_name(
    std::string_view sql, std::span<const unsigned int> types, std::pmr::memory_resource* resource) {
    std::pmr::string key(resource);
    key.reserve(sql.size() + 1 + types.size() * 4);
    key.append(sql);
    key.push_back('\0');
    for (const auto type : types) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            key.push_back(static_cast<char>((type >> shift) & 0xffU));
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), digest.data()) == nullptr) {
        throw std::runtime_error("PostgreSQL statement name digest failed");
    }
    constexpr std::string_view prefix{"ruvia_"};
    postgresql_statement_cache::name_type name{};
    prefix.copy(name.data(), prefix.size());
    // Zero initialization leaves the terminating NUL after the hex digits.
    http_encode_lower_hex(name.data() + prefix.size(), std::span(digest.data(), (name.size() - prefix.size()) / 2));
    return name;
}

}  // namespace

db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type error_code, const pg_result* result_value) {
    // libpq marks the connection bad when the transport fails, and reports
    // that failure as a fatal result; it is an I/O failure, not a statement
    // the server rejected.
    if (error_code == db_error::code_type::statement_failed && PQstatus(&connection) == CONNECTION_BAD) {
        error_code = db_error::code_type::io_error;
    }
    std::string error(operation);
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
    return db_error(error_code, db_driver::postgresql, error, std::nullopt,
        state_value == nullptr ? std::string_view{} : std::string_view(state_value),
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
    output.values_.reserve(params.size());
    output.types_.reserve(params.size());

    for (const auto& param : params) {
        const auto type = db_value_access::type(param);
        output.types_.push_back(type == db_value_type::double_value ? postgresql_float8_oid : 0U);
        const char* text = nullptr;
        switch (type) {
            case db_value_type::null:
                break;
            case db_value_type::string: {
                const auto& value = db_value_access::owned_text(param);
                if (value.find('\0') != std::string_view::npos) {
                    throw std::invalid_argument("PostgreSQL string parameter must not contain NUL bytes");
                }
                if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                    throw std::length_error("PostgreSQL parameter is too large");
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
                // Reserve before publishing the first number's c_str: vector
                // growth would invalidate pointers into short string storage.
                if (output.encoded_.empty()) {
                    output.encoded_.reserve(params.size());
                }
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

postgresql_statement_cache::postgresql_statement_cache(std::pmr::memory_resource* resource)
    : entries_(pmr_resource_or_default(resource)) {}

postgresql_statement_cache::lookup postgresql_statement_cache::find_or_insert(
    std::string_view sql, std::span<const unsigned int> types) {
    const auto hash = postgresql_statement_hash(sql, types);
    ++clock_;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        auto& existing = entries_[index];
        if (existing.hash_ == hash && existing.sql_ == sql && std::ranges::equal(existing.types_, types)) {
            existing.last_use_ = clock_;
            return {index};
        }
    }
    const auto first_use = std::ranges::find(first_uses_, hash);
    if (first_use == first_uses_.end()) {
        first_uses_[next_first_use_] = hash;
        next_first_use_ = (next_first_use_ + 1) % first_uses_.size();
        return {};
    }
    *first_use = 0;
    // Build the whole entry before touching the table, so a failed allocation
    // leaves every entry, and the server statement it names, intact.
    auto* resource = entries_.get_allocator().resource();
    entry added{std::pmr::string(sql, resource), std::pmr::vector<unsigned int>(types.begin(), types.end(), resource),
        hash, clock_, postgresql_statement_name(sql, types, resource)};
    if (entries_.size() < capacity) {
        entries_.push_back(std::move(added));
        return {entries_.size() - 1, true, std::nullopt};
    }
    const auto oldest = std::ranges::min_element(entries_, {}, &entry::last_use_);
    lookup result{static_cast<std::size_t>(oldest - entries_.begin()), true, oldest->name_};
    *oldest = std::move(added);
    return result;
}

void postgresql_statement_cache::forget(std::size_t index) {
    if (index + 1 != entries_.size()) {
        entries_[index] = std::move(entries_.back());
    }
    entries_.pop_back();
}

void postgresql_statement_cache::clear() noexcept {
    entries_.clear();
    first_uses_.fill(0);
}

}  // namespace ruvia::detail
