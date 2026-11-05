#pragma once

#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/web/db/db.h"

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.

namespace ruvia {

struct prepared_db_statement final {
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
};

// Raw SQL is checked once, where the caller hands it over: it must hold a
// statement, and no NUL byte, since both client libraries read SQL as a C
// string. Compiled SQL is built that way; parameters are checked where they
// are encoded for the client library.
void validate_db_sql_text(std::string_view sql);

[[nodiscard]] prepared_db_statement prepare_db_statement(
    std::string_view sql, std::span<const db_value> params, std::pmr::memory_resource* resource);

// PostgreSQL NOTIFY as a pg_notify() call, so channel and payload travel as
// bound parameters rather than as SQL text.
[[nodiscard]] prepared_db_statement prepare_db_notify(
    std::string_view channel, std::string_view payload, std::pmr::memory_resource* resource);

}  // namespace ruvia
