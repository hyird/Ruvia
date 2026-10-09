#include "ruvia/web/db/db_query_result_cache.h"

#include <optional>
#include <utility>

#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif

namespace ruvia::detail {
#ifdef RUVIA_ENABLE_REDIS
namespace {
struct redis_cache_store {
    redis_handle redis_;
    auto get(std::string_view key, operation_options options) {
        return redis_.with_options(std::move(options)).get(key);
    }
    auto put(std::string_view key, std::string_view value, std::chrono::milliseconds duration,
        operation_options options) {
        return redis_.with_options(std::move(options)).set(key, value, {.expiration_ = redis_set_expiration::expires_after(duration)});
    }
};
task<void> remove_keys(redis_handle redis, std::pmr::vector<std::pmr::string> keys,
    operation_options options) {
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    for (const auto& key : keys) {
        try {
            co_await redis.with_options(db_cache_required_options(options, operation_timeout_value)).del(key);
        } catch (const redis_error& error) {
            if (error.code() == redis_error::code_type::cancelled ||
                error.code() == redis_error::code_type::closing) {
                throw;
            }
            if (operation_timeout_value.expired()) {
                throw_db_cache_timeout();
            }
            throw;
        }
        if (operation_timeout_value.expired()) {
            throw_db_cache_timeout();
        }
    }
}
task<void> clear_keys(redis_handle redis, std::pmr::string pattern, operation_options options) {
    const ruvia::operation_timeout operation_timeout_value(options.timeout_);
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_cache_required_options(options, operation_timeout_value)).scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
        } catch (const redis_error& error) {
            if (error.code() == redis_error::code_type::cancelled ||
                error.code() == redis_error::code_type::closing) {
                throw;
            }
            if (operation_timeout_value.expired()) {
                throw_db_cache_timeout();
            }
            throw;
        }
        if (operation_timeout_value.expired()) {
            throw_db_cache_timeout();
        }
        for (const auto& key : result->values()) {
            try {
                co_await redis.with_options(db_cache_required_options(options, operation_timeout_value)).del(key);
            } catch (const redis_error& error) {
                if (error.code() == redis_error::code_type::cancelled ||
                    error.code() == redis_error::code_type::closing) {
                    throw;
                }
                if (operation_timeout_value.expired()) {
                    throw_db_cache_timeout();
                }
                throw;
            }
            if (operation_timeout_value.expired()) {
                throw_db_cache_timeout();
            }
        }
        cursor = result->next_cursor();
    } while (cursor);
}
}  // namespace
#endif
db_query_cache_state::db_query_cache_state(const redis_handle& redis,
    const db_cache_config_storage& config, std::string_view identity, std::pmr::memory_resource* resource)
    : resource_(resource),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      name_space_(identity, resource)
#ifdef RUVIA_ENABLE_REDIS
      ,
      redis_(redis.with_options({}))
#endif
{
#ifndef RUVIA_ENABLE_REDIS
    (void)redis;
    throw std::invalid_argument("database caching requires Redis support");
#endif
}
db_query_cache_state::~db_query_cache_state() = default;
void db_query_cache_state::close_now() noexcept {
    closed_ = true;
}
void db_query_cache_state::require_open() const {
    if (closed_) {
        throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
    }
}
std::optional<std::pmr::string> db_query_cache_state::key(const db_query& query, const db_statement& statement, db_driver driver) {
    if (!query.cache_enabled().value_or(always_enabled_) || !query.cacheable()) {
        return std::nullopt;
    }
    return db_cache_key(name_space_, query.cache_id(), statement.sql(), statement.params(), driver, resource_);
}
task<db_rows> db_query_cache_state::wrap(std::optional<std::chrono::milliseconds> duration, std::optional<std::pmr::string> key,
    db_cache_query database_value, operation_options options,
    std::optional<ruvia::operation_timeout> deadline_value) {
#ifdef RUVIA_ENABLE_REDIS
    require_open();
    if (key) {
        return query_db_cache(redis_cache_store{redis_}, std::move(*key),
            duration.value_or(duration_), ignore_errors_, std::move(database_value), resource_,
            std::move(options), std::move(deadline_value));
    }
#else
    (void)duration;
    (void)key;
    (void)deadline_value;
#endif
    return std::move(database_value)(std::move(options));
}
task<void> db_query_cache_state::remove(std::span<const std::string_view> ids, operation_options options) {
#ifdef RUVIA_ENABLE_REDIS
    require_open();
    std::pmr::vector<std::pmr::string> keys(resource_);
    keys.reserve(ids.size());
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
        keys.push_back(db_cache_key(name_space_, id, {}, {}, db_driver::unspecified, resource_));
    }
    return remove_keys(redis_, std::move(keys), std::move(options));
#else
    (void)ids;
    (void)options;
    throw std::logic_error("Redis support is disabled");
#endif
}
task<void> db_query_cache_state::clear(operation_options options) {
#ifdef RUVIA_ENABLE_REDIS
    require_open();
    auto pattern = db_cache_prefix(name_space_, resource_);
    pattern.push_back('*');
    return clear_keys(redis_, std::move(pattern), std::move(options));
#else
    (void)options;
    throw std::logic_error("Redis support is disabled");
#endif
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
    return make_scoped_operation(registration_.scope(), state_->remove(ids, options_));
}
scoped_operation<void> db_query_result_cache::clear() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->clear(options_));
}
}  // namespace ruvia
