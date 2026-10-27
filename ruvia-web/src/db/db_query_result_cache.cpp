#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/detail/integration/request_io_metrics.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"

namespace ruvia::detail {
namespace {

bool is_fatal_redis_cache_error(const redis_error& error) noexcept {
    return error.code() == redis_error::code_type::cancelled ||
           error.code() == redis_error::code_type::closing;
}

void rethrow_or_timeout_redis_cache_error(
    const redis_error& error, const operation_timeout& deadline, bool ignore_errors) {
    if (is_fatal_redis_cache_error(error)) {
        throw;
    }
    if (deadline.expired()) {
        throw_db_query_timeout();
    }
    if (!ignore_errors) {
        throw;
    }
}
task<void> delete_key_batch(const redis_handle& redis, std::span<const std::pmr::string> keys,
    std::pmr::memory_resource* resource, const operation_options& options, const operation_timeout& deadline_value) {
    if (keys.empty()) {
        co_return;
    }
    std::pmr::vector<std::string_view> views(resource);
    views.reserve(keys.size());
    for (const auto& key : keys) {
        views.emplace_back(key);
    }
    try {
        (void)co_await redis.with_options(db_required_options(options, deadline_value)).del(views);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
    }
    if (deadline_value.expired()) {
        throw_db_query_timeout();
    }
}

task<void> remove_keys(
    redis_handle redis, std::pmr::vector<std::pmr::string> keys, operation_scope& scope, operation_options options) {
    scoped_request_io_timer timer(scope, request_io_kind::redis);
    const operation_timeout deadline_value(options.timeout_);
    co_await delete_key_batch(redis, keys, keys.get_allocator().resource(), options, deadline_value);
}

task<void> clear_keys(
    redis_handle redis, std::pmr::string pattern, operation_scope& scope, operation_options options) {
    scoped_request_io_timer timer(scope, request_io_kind::redis);
    const operation_timeout deadline_value(options.timeout_);
    auto* const resource = pattern.get_allocator().resource();
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_required_options(options, deadline_value))
                    .scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        }
        if (deadline_value.expired()) {
            throw_db_query_timeout();
        }
        co_await delete_key_batch(redis, result->values(), resource, options, deadline_value);
        cursor = result->next_cursor();
    } while (cursor);
}

}  // namespace

db_query_cache_state::db_query_cache_state(const redis_handle& redis, const db_cache_config_storage& config,
    std::string_view identity, std::pmr::memory_resource* resource)
    : resource_(resource),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      prefix_(db_cache_prefix(identity, resource)),
      redis_(redis.with_options({})) {}

db_query_cache_state::~db_query_cache_state() = default;

void db_query_cache_state::close_now() noexcept {
    closed_ = true;
}

void db_query_cache_state::require_open() const {
    if (closed_) {
        throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
    }
}

std::optional<std::pmr::string> db_query_cache_state::key(
    const db_query& query, const db_statement& statement, db_driver driver) {
    if (!query.cache_enabled().value_or(always_enabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return db_cache_key(prefix_, query.cache_id(), query.cache_shape(), statement.sql(), statement.params(), driver,
        resource_);
}

task<db_rows> db_query_cache_state::wrap(std::optional<std::chrono::milliseconds> duration, std::pmr::string key,
    db_cache_query database, operation_options options, operation_timeout deadline) {
    require_open();
    return query(std::move(key), duration.value_or(duration_), std::move(database), std::move(options), deadline);
}

// Key, deferred database statement and options belong to this cold frame. On
// a hit the statement is destroyed without starting. Decoded rows own their
// fields independently of the Redis reply and later cache operations.
task<db_rows> db_query_cache_state::query(std::pmr::string key, std::chrono::milliseconds duration,
    db_cache_query database, operation_options options, operation_timeout deadline) {
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::pmr::string> bytes_value;
    try {
        bytes_value = co_await redis_.with_options(db_required_options(options, deadline)).get(key);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
    }
    if (bytes_value.has_value()) {
        std::optional<db_rows> rows;
        try {
            rows.emplace(decode_db_cache_rows(*bytes_value, resource_));
        } catch (const db_conversion_error&) {
            if (deadline.expired()) {
                throw_db_query_timeout();
            }
            if (!ignore_errors_) {
                throw;
            }
        }
        if (rows.has_value()) {
            if (deadline.expired()) {
                throw_db_query_timeout();
            }
            co_return std::move(*rows);
        }
    }
    if (deadline.expired()) {
        throw_db_query_timeout();
    }
    auto rows = co_await std::move(database)(db_remaining_options(options, deadline));
    if (deadline.expired()) {
        throw_db_query_timeout();
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    if (elapsed < duration) {
        const auto encoded_bytes = encode_db_cache_rows(rows, resource_);
        try {
            (void)co_await redis_.with_options(db_required_options(options, deadline))
                .set(key, encoded_bytes, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)});
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (deadline.expired()) {
            throw_db_query_timeout();
        }
    }
    co_return rows;
}

// A cache id names every result shape a repository builder derived from it.
task<void> db_query_cache_state::remove(
    std::span<const std::string_view> ids, operation_scope& scope, operation_options options) {
    require_open();
    std::pmr::vector<std::pmr::string> keys(resource_);
    keys.reserve(ids.size() * db_cache_shapes.size());
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
        for (const auto shape : db_cache_shapes) {
            keys.push_back(db_cache_id_key(prefix_, id, shape, resource_));
        }
    }
    return remove_keys(redis_, std::move(keys), scope, std::move(options));
}

task<void> db_query_cache_state::clear(operation_scope& scope, operation_options options) {
    require_open();
    std::pmr::string pattern(resource_);
    pattern.reserve(prefix_.size() + 1);
    pattern.append(prefix_);
    pattern.push_back('*');
    return clear_keys(redis_, std::move(pattern), scope, std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia {
db_query_result_cache::db_query_result_cache(detail::db_query_cache_state& state_value, operation_scope& scope, operation_options options) noexcept
    : state_(&state_value),
      options_(std::move(options)),
      registration_(scope, this, &db_query_result_cache::expire_capability) {}

db_query_result_cache::db_query_result_cache(const db_query_result_cache& other) noexcept
    : state_(other.state_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void db_query_result_cache::expire_capability(void* target) noexcept {
    auto& cache = *static_cast<db_query_result_cache*>(target);
    cache.state_ = nullptr;
    cache.options_ = {};
}
task<void> db_query_result_cache::remove(std::span<const std::string_view> ids) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return make_scoped_operation(scope, state_->remove(ids, scope, options_));
}
task<void> db_query_result_cache::clear() const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return make_scoped_operation(scope, state_->clear(scope, options_));
}
}  // namespace ruvia
