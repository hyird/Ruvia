#pragma once

#include <array>
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

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia {
class db_query;
}

namespace ruvia::detail {

struct db_config_storage;
class db_query_cache_state;

struct db_query_step final {
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
    std::optional<std::pmr::string> cache_key_;
    std::optional<std::chrono::milliseconds> cache_duration_;
};

// Preparation owns every statement and cache key before returning a cold task.
// Transaction state and its exclusive lease remain with the outer operation.
struct db_query_plan final {
    db_query_step first_;
    std::optional<db_query_step> second_;
    [[nodiscard]] static db_query_plan prepare(const db_query& query,
        const db_query* count, db_driver driver, std::pmr::memory_resource* resource,
        db_query_cache_state* cache);
};

struct db_query_backend final {
    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    std::pmr::memory_resource* resource_;
    db_query_cache_state* cache_;
    bool* backend_failed_{nullptr};

    // Move the inputs into db_cache_query before creating a lazy backend task.
    [[nodiscard]] task<db_rows> operator()(db_query_step step,
        operation_options options, const operation_timeout& deadline) const;
};

template <bool with_count>
using db_query_result = std::conditional_t<with_count, std::pair<db_rows, db_rows>, db_rows>;

[[nodiscard]] std::pmr::string db_cache_scope(std::string_view name_space,
    std::string_view alias, const db_config_storage& config, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource);
[[nodiscard]] db_rows decode_db_cache_rows(std::string_view bytes_value, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string db_cache_key(std::string_view name_space, std::string_view id,
    std::string_view sql, std::span<const db_value> params, db_driver driver,
    std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string db_cache_prefix(std::string_view name_space, std::pmr::memory_resource* resource);

class db_cache_query final {
public:
    db_cache_query(db_pool_ref_type pool, std::optional<std::size_t> slot,
        std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, bool* backend_failed = nullptr) noexcept
        : pool_(pool),
          slot_(slot),
          sql_(std::move(sql)),
          params_(std::move(params)),
          resource_(pmr_resource_or_default(resource)),
          backend_failed_(backend_failed) {}

    db_cache_query(const db_cache_query&) = delete;
    db_cache_query& operator=(const db_cache_query&) = delete;
    db_cache_query(db_cache_query&&) noexcept = default;
    db_cache_query& operator=(db_cache_query&&) = delete;

    // This is deliberately not a coroutine. Moving the owned statement into
    // execute_owned() makes the options local to that coroutine frame before a
    // lazy transaction backend task borrows them.
    [[nodiscard]] task<db_rows> operator()(operation_options options) &&;

private:
    static task<db_rows> execute_owned(db_pool_ref_type pool, std::optional<std::size_t> slot,
        std::pmr::string sql, std::pmr::vector<db_value> params,
        std::pmr::memory_resource* resource, bool* backend_failed,
        operation_options options);

    db_pool_ref_type pool_;
    std::optional<std::size_t> slot_;
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
    std::pmr::memory_resource* resource_;
    bool* backend_failed_;
};

[[nodiscard]] inline operation_options db_cache_remaining_options(
    const operation_options& base, const ruvia::operation_timeout& timeout) {
    auto result_value = base;
    result_value.timeout_ = timeout.remaining();
    return result_value;
}

[[noreturn]] inline void throw_db_cache_timeout() {
    throw db_error(db_error::code_type::timeout, std::nullopt,
        "database query cache operation timed out");
}

[[nodiscard]] inline operation_options db_cache_required_options(
    const operation_options& base, const ruvia::operation_timeout& timeout) {
    auto result_value = db_cache_remaining_options(base, timeout);
    if (result_value.timeout_.has_value() && result_value.timeout_->count() == 0) {
        throw_db_cache_timeout();
    }
    return result_value;
}
// One deadline and one execution chain for direct, cached, and mixed sequences.
// The backend is a value adapter, not a transaction owner or type-erased callback.
template <bool with_count, typename backend_type>
task<db_query_result<with_count>> execute_db_query_plan(
    db_query_plan plan, backend_type backend, operation_options options) {
    const operation_timeout deadline_value(options.timeout_);
    std::array<std::optional<db_rows>, with_count ? 2 : 1> results;
    for (std::size_t index = 0; index < results.size(); ++index) {
        auto& step = index == 0 ? plan.first_ : plan.second_.value();
        results[index].emplace(co_await backend(std::move(step),
            db_cache_required_options(options, deadline_value), deadline_value));
        if (deadline_value.expired()) {
            throw_db_cache_timeout();
        }
    }
    if constexpr (with_count) {
        co_return std::pair{std::move(*results[0]), std::move(*results[1])};
    } else {
        co_return std::move(*results[0]);
    }
}

// Store, key, and deferred database query belong to the cold frame. On a hit
// the query object is destroyed without starting. Decoded results own their
// fields independently of both Redis replies and subsequent cache operations.
template <typename store, typename database>
task<db_rows> query_db_cache(store store_value, std::pmr::string key, std::chrono::milliseconds duration,
    bool ignore_errors, database database_value, std::pmr::memory_resource* resource,
    operation_options options, std::optional<ruvia::operation_timeout> deadline = std::nullopt) {
    const auto started = std::chrono::steady_clock::now();
    const auto operation_timeout_value = deadline.has_value()
                                             ? *deadline
                                             : ruvia::operation_timeout(options.timeout_);
    try {
        auto cached = co_await store_value.get(key, db_cache_required_options(options, operation_timeout_value));
        if (cached) {
            auto rows = decode_db_cache_rows(*cached, resource);
            if (operation_timeout_value.expired()) {
                throw_db_cache_timeout();
            }
            co_return rows;
        }
    } catch (const redis_error& error) {
        if (error.code() == redis_error::code_type::cancelled ||
            error.code() == redis_error::code_type::closing) {
            throw;
        }
        if (operation_timeout_value.expired()) {
            throw_db_cache_timeout();
        }
        if (!ignore_errors) {
            throw;
        }
    } catch (const db_conversion_error&) {
        if (operation_timeout_value.expired()) {
            throw_db_cache_timeout();
        }
        if (!ignore_errors) {
            throw;
        }
    }
    if (operation_timeout_value.expired()) {
        throw_db_cache_timeout();
    }
    auto rows = co_await std::move(database_value)(db_cache_remaining_options(options, operation_timeout_value));
    if (operation_timeout_value.expired()) {
        throw_db_cache_timeout();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed < duration) {
        auto bytes_value = encode_db_cache_rows(rows, resource);
        try {
            co_await store_value.put(key, bytes_value, duration - elapsed,
                db_cache_required_options(options, operation_timeout_value));
        } catch (const redis_error& error) {
            if (error.code() == redis_error::code_type::cancelled ||
                error.code() == redis_error::code_type::closing) {
                throw;
            }
            if (operation_timeout_value.expired()) {
                throw_db_cache_timeout();
            }
            if (!ignore_errors) {
                throw;
            }
        }
        if (operation_timeout_value.expired()) {
            throw_db_cache_timeout();
        }
    }
    if (operation_timeout_value.expired()) {
        throw_db_cache_timeout();
    }
    co_return rows;
}

}  // namespace ruvia::detail
