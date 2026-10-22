#pragma once

#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"

// Execution of compiled db_query statements: one deadline and one chain for a
// query, or a query and its count, run directly or through the result cache.

namespace ruvia {
class db_query;
}

namespace ruvia::detail {

#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

// Where a plan's statements run: a pooled connection, or the slot an open
// transaction holds. A value adapter, not a lease owner.
struct db_query_backend final {
    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    std::pmr::memory_resource* resource_;
    operation_scope* scope_{nullptr};
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
    const operation_options& base, const operation_timeout& timeout) {
    auto result_value = base;
    result_value.timeout_ = timeout.remaining();
    return result_value;
}

[[noreturn]] inline void throw_db_query_timeout() {
    throw db_error(db_error::code_type::timeout, std::nullopt, "database query cache operation timed out");
}

[[nodiscard]] inline operation_options db_required_options(
    const operation_options& base, const operation_timeout& timeout) {
    auto result_value = db_remaining_options(base, timeout);
    if (result_value.timeout_.has_value() && result_value.timeout_->count() == 0) {
        throw_db_query_timeout();
    }
    return result_value;
}

}  // namespace ruvia::detail
