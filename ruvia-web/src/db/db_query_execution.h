#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
// Execution of compiled db_query statements: one deadline and one chain for a
// query, or a query and its count, run directly or through the result cache.

namespace ruvia {
class db_query;

// A statement copied into the worker's memory resource so it outlives the
// caller's arguments: a suspended query keeps borrowing its SQL and parameters
// long after the expression that produced them has ended.
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

namespace detail {

[[nodiscard]] inline db_value clone_db_value_for_resource(
    const db_value& value, std::pmr::memory_resource* resolved_resource) {
    switch (value.type()) {
        case db_value_type::null:
            return db_value(nullptr);
        case db_value_type::string:
            return db_value_access::owned_string(
                std::pmr::string(value.text(), resolved_resource));
        case db_value_type::signed_value:
            return db_value(value.signed_value());
        case db_value_type::unsigned_value:
            return db_value(value.unsigned_value());
        case db_value_type::double_value:
            return db_value(value.double_value());
        case db_value_type::bool_value:
            return db_value(value.bool_value());
    }
    return db_value(nullptr);
}

[[nodiscard]] inline std::pmr::vector<db_value> clone_db_values(
    std::span<const db_value> values, std::pmr::memory_resource* resource) {
    auto* resolved = pmr_resource_or_default(resource);
    std::pmr::vector<db_value> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(clone_db_value_for_resource(value, resolved));
    }
    return output;
}

struct request_io_metrics;

#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

// Where a plan's statements run: a pooled connection, or the slot an open
// transaction holds. A value adapter, not a lease owner.
struct db_query_backend final {
    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    std::pmr::memory_resource* resource_;
    // The request's counter block; null outside a request.
    request_io_metrics* io_metrics_{nullptr};
    // Set when the database statement itself failed: a transaction statement
    // failure has already retired the transaction's lease.
    bool* backend_failed_{nullptr};
#ifdef RUVIA_ENABLE_REDIS
    // Pooled backends only: a transaction's reads may include its own
    // uncommitted writes, which the shared cache must never see or replace.
    db_query_cache_state* cache_{nullptr};
#endif
};

struct db_query_step final {
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
#ifdef RUVIA_ENABLE_REDIS
    // Present only when a configured cache applies to this statement.
    std::optional<std::pmr::string> cache_key_;
    std::optional<std::chrono::milliseconds> cache_duration_;
#endif
};

// Preparation owns every statement and cache key before returning a cold task.
// Transaction state and its exclusive lease remain with the outer operation.
struct db_query_plan final {
    db_query_step first_;
    std::optional<db_query_step> second_;
    [[nodiscard]] static db_query_plan prepare(
        const db_query& query, const db_query* count, const db_query_backend& backend);
};

template <bool with_count>
using db_query_result = std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>;

template <bool with_count>
[[nodiscard]] task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, db_query_backend backend, operation_options options);

// Runs one statement on the backend without a cache. `options` is owned by the
// awaiting frame, which must outlive the returned task; a pooled statement
// takes it over, a transaction statement borrows it.
[[nodiscard]] task<db_rows> query_db_backend(const db_query_backend& backend, std::pmr::string sql,
    std::pmr::vector<db_value> params, operation_options& options);

[[nodiscard]] inline operation_options db_remaining_options(
    const operation_options& base, const operation_deadline& timeout) {
    auto result_value = base;
    result_value.timeout_ = timeout.timeout();
    return result_value;
}

}  // namespace detail
}  // namespace ruvia
