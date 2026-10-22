#include "ruvia/web/db/db_query_result_cache.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/web/detail/integration/request_io_metrics.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_cache_shape.h"
#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"

namespace ruvia::detail {
namespace {

template <typename operation_type>
struct scoped_operation_value;

template <typename value_type>
struct scoped_operation_value<scoped_operation<value_type>> {
    using type = value_type;
};

template <typename value_type>
using redis_cache_outcome = std::variant<value_type, std::exception_ptr>;

// The one Redis failure rule for cache operations: cancellation and shutdown
// propagate unchanged, any failure once the cache deadline has passed is the
// cache timeout, and other failures are handed back for the caller's policy.
// `command` builds the Redis operation inside this frame, so the handle it is
// issued through lives until the operation completes.
template <typename command_type,
    typename value_type = typename scoped_operation_value<std::invoke_result_t<command_type&>>::type>
task<redis_cache_outcome<value_type>> run_redis_cache_command(command_type command, const operation_timeout& deadline) {
    std::exception_ptr failure;
    try {
        co_return redis_cache_outcome<value_type>(std::in_place_index<0>, co_await command());
    } catch (const redis_error& error) {
        if (error.code() == redis_error::code_type::cancelled || error.code() == redis_error::code_type::closing) {
            throw;
        }
        failure = std::current_exception();
    }
    if (deadline.expired()) {
        throw_db_query_timeout();
    }
    co_return redis_cache_outcome<value_type>(std::in_place_index<1>, std::move(failure));
}

// Maintenance commands never tolerate a store failure.
template <typename value_type>
value_type require_redis_cache_value(redis_cache_outcome<value_type> outcome) {
    if (auto* failure = std::get_if<std::exception_ptr>(&outcome)) {
        std::rethrow_exception(*failure);
    }
    return std::get<0>(std::move(outcome));
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
    (void)require_redis_cache_value(co_await run_redis_cache_command(
        [&] { return redis.with_options(db_required_options(options, deadline_value)).del(views); },
        deadline_value));
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
        auto result = require_redis_cache_value(co_await run_redis_cache_command(
            [&] {
                return redis.with_options(db_required_options(options, deadline_value))
                    .scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256});
            },
            deadline_value));
        if (deadline_value.expired()) {
            throw_db_query_timeout();
        }
        co_await delete_key_batch(redis, result.values(), resource, options, deadline_value);
        cursor = result.next_cursor();
    } while (cursor);
}

}  // namespace

db_query_cache_state::db_query_cache_state(const redis_handle& redis, const db_cache_config_storage& config,
    std::string_view identity, std::pmr::memory_resource* resource)
    : resource_(resource),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      name_space_(identity, resource),
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
    return db_cache_key(name_space_, query.cache_id(), query.cache_shape(), statement.sql(), statement.params(), driver,
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
    auto cached = co_await run_redis_cache_command(
        [&] { return redis_.with_options(db_required_options(options, deadline)).get(key); }, deadline);
    if (auto* failure = std::get_if<std::exception_ptr>(&cached)) {
        if (!ignore_errors_) {
            std::rethrow_exception(*failure);
        }
    } else if (const auto& bytes_value = std::get<0>(cached); bytes_value.has_value()) {
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
        const auto bytes_value = encode_db_cache_rows(rows, resource_);
        const auto stored = co_await run_redis_cache_command(
            [&] {
                return redis_.with_options(db_required_options(options, deadline))
                    .set(key, bytes_value, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)});
            },
            deadline);
        if (const auto* failure = std::get_if<std::exception_ptr>(&stored); failure != nullptr && !ignore_errors_) {
            std::rethrow_exception(*failure);
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
            keys.push_back(db_cache_id_key(name_space_, id, shape, resource_));
        }
    }
    return remove_keys(redis_, std::move(keys), scope, std::move(options));
}

task<void> db_query_cache_state::clear(operation_scope& scope, operation_options options) {
    require_open();
    auto pattern = db_cache_prefix(name_space_, resource_);
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
scoped_operation<void> db_query_result_cache::remove(std::span<const std::string_view> ids) const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return make_scoped_operation(scope, state_->remove(ids, scope, options_));
}
scoped_operation<void> db_query_result_cache::clear() const {
    registration_.require_active();
    auto& scope = registration_.scope();
    return make_scoped_operation(scope, state_->clear(scope, options_));
}
}  // namespace ruvia
