#pragma once

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db_types.h"

struct pg_conn;
struct pg_result;

namespace ruvia::detail {

// Builds the error for a failed libpq call or result. A statement_failed on a
// connection libpq has marked bad is reported as io_error.
[[nodiscard]] db_error postgresql_error(const pg_conn& connection, std::string_view operation,
    db_error::code_type code, const pg_result* result = nullptr);

// Parameters are sent in text format. A double declares float8 so the server
// never infers an integer type from an integral value; every other parameter
// is left untyped (OID 0): the SQL decides its type, and compiled SQL casts
// each placeholder from its value category.
struct postgresql_params final {
    explicit postgresql_params(std::pmr::memory_resource* resource);

    std::pmr::vector<std::pmr::string> encoded_;
    std::pmr::vector<const char*> values_;
    // libpq's Oid is unsigned int; 0 leaves the type to the server.
    std::pmr::vector<unsigned int> types_;
};

// Borrows owned string parameters from the prepared statement. The caller must
// keep params alive and unchanged through PQsendQueryParams; numeric encodings
// are owned by the returned object. No text-format lengths array is needed.
[[nodiscard]] postgresql_params encode_postgresql_params(
    std::span<const db_value> params, std::pmr::memory_resource* resource);

[[nodiscard]] std::uint64_t postgresql_affected_rows(const pg_result& result_value) noexcept;

}  // namespace ruvia::detail
