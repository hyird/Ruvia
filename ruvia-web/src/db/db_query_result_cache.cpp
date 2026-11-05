#include <array>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/timer.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_query_cache.h"
#include "db/db_query_cache_state.h"
#include "integration/request_io_metrics.h"
#include "redis/redis_client_runtime.h"
#include "redis/redis_protocol.h"

namespace ruvia::detail {
namespace {

[[noreturn]] void throw_db_cache_timeout() {
    throw db_error(db_error::code_type::timeout, std::nullopt, "database query cache operation timed out");
}

// The options of one Redis operation under the deadline it shares with the
// query it caches.
[[nodiscard]] operation_options db_cache_options(const operation_options& base, const operation_timeout& timeout) {
    auto result_value = db_remaining_options(base, timeout);
    if (result_value.timeout_.has_value() && result_value.timeout_->count() == 0) {
        throw_db_cache_timeout();
    }
    return result_value;
}

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
        throw_db_cache_timeout();
    }
    if (!ignore_errors) {
        throw;
    }
}

// Rows of a cached entry, or nullopt for a corrupt entry the cache ignores.
// The bytes are released before the caller goes on to the database.
[[nodiscard]] std::optional<db_rows> decode_cached_rows(std::pmr::string bytes_value,
    std::pmr::memory_resource* resource, const operation_timeout& deadline, bool ignore_errors) {
    std::optional<db_rows> rows;
    try {
        rows.emplace(decode_db_cache_rows(std::move(bytes_value), resource));
    } catch (const db_conversion_error&) {
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        if (!ignore_errors) {
            throw;
        }
        return std::nullopt;
    }
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    return rows;
}

task<void> finish_delete_key_batch(std::optional<task<std::int64_t>> deletion, const operation_timeout& deadline_value) {
    if (!deletion) {
        co_return;
    }
    try {
        (void)co_await std::move(*deletion);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
    }
    if (deadline_value.expired()) {
        throw_db_cache_timeout();
    }
}

task<void> delete_key_batch(const redis_handle& redis, std::span<const std::pmr::string> keys,
    std::pmr::memory_resource* resource, const operation_options& options, const operation_timeout& deadline_value) {
    if (keys.empty()) {
        return finish_delete_key_batch(std::nullopt, deadline_value);
    }
    try {
        static constexpr std::size_t inline_capacity = 64;
        if (keys.size() <= inline_capacity) {
            std::array<std::string_view, inline_capacity> inline_views;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                inline_views[i] = keys[i];
            }
            // DEL owns its arguments before returning its cold operation.
            auto deletion = redis.with_options(db_cache_options(options, deadline_value))
                                .del(std::span<const std::string_view>(inline_views.data(), keys.size()));
            return finish_delete_key_batch(std::move(deletion), deadline_value);
        }
        std::pmr::vector<std::string_view> heap_views(resource);
        heap_views.reserve(keys.size());
        for (const auto& key : keys) {
            heap_views.emplace_back(key);
        }
        auto deletion = redis.with_options(db_cache_options(options, deadline_value)).del(heap_views);
        return finish_delete_key_batch(std::move(deletion), deadline_value);
    } catch (const redis_error& error) {
        rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        throw;
    }
}

task<void> remove_keys(redis_handle redis, std::pmr::vector<std::pmr::string> keys, request_io_metrics* io_metrics,
    operation_options options, stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_timeout deadline_value(options.timeout_);
    auto deletion = delete_key_batch(redis, keys, keys.get_allocator().resource(), options, deadline_value);
    // The command owns the keys; release their source before network IO.
    std::pmr::vector<std::pmr::string>(keys.get_allocator().resource()).swap(keys);
    co_await std::move(deletion);
}

task<void> clear_keys(
    redis_handle redis, std::pmr::string pattern, request_io_metrics* io_metrics, operation_options options,
    stop_token shutdown) {
    stop_group cancellation{options.stop_token_, std::move(shutdown)};
    options.stop_token_ = cancellation.token();
    scoped_request_io_timer timer(io_metrics, request_io_kind::redis);
    const operation_timeout deadline_value(options.timeout_);
    auto* const resource = pattern.get_allocator().resource();
    std::optional<redis_scan_cursor> cursor;
    do {
        std::optional<redis_scan_result> result;
        try {
            result.emplace(co_await redis.with_options(db_cache_options(options, deadline_value))
                    .scan({.cursor_ = cursor, .match_ = std::string_view(pattern), .count_ = 256}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline_value, false);
        }
        if (deadline_value.expired()) {
            throw_db_cache_timeout();
        }
        cursor = result->next_cursor();
        auto deletion = delete_key_batch(redis, result->values(), resource, options, deadline_value);
        // Preserve only the cursor while the owned DEL waits for its reply.
        result.reset();
        co_await std::move(deletion);
    } while (cursor);
}

}  // namespace

db_query_cache_state::db_query_cache_state(const redis_handle& redis, const db_cache_config_storage& config,
    std::pmr::string prefix, std::pmr::memory_resource* resource)
    : resource_(resource),
      worker_(redis.worker()),
      duration_(config.duration_),
      always_enabled_(config.always_enabled_),
      ignore_errors_(config.ignore_errors_),
      max_entry_reply_bytes_(redis_client_runtime::handle_config(redis).max_reply_bytes_),
      prefix_(std::move(prefix)),
      redis_(redis.with_options({})),
      fills_(resource) {}

db_query_cache_state::~db_query_cache_state() = default;

// The leader's hold on the fill it listed. Every exit -- a result, a failure,
// a cancellation or a timeout -- unlists the fill and ends it, so its waiters
// never outlive the leader's attempt.
class db_query_cache_state::fill_lead final {
public:
    fill_lead(db_query_cache_state& state, fill_pointer listed) noexcept
        : state_(state),
          fill_(std::move(listed)) {}
    fill_lead(const fill_lead&) = delete;
    fill_lead& operator=(const fill_lead&) = delete;
    ~fill_lead() {
        finish();
    }

    [[nodiscard]] fill& get() const noexcept {
        return *fill_;
    }
    void finish() noexcept {
        if (!finished_) {
            finished_ = true;
            state_.unlist_fill(*fill_);
            fill_->done_.request_stop();
        }
    }

private:
    db_query_cache_state& state_;
    fill_pointer fill_;
    bool finished_{false};
};

void db_query_cache_state::close_now() noexcept {
    closed_ = true;
    stop_.request_stop();
}

void db_query_cache_state::require_open() const {
    if (closed_) {
        throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
    }
}

void db_query_cache_state::unlist_fill(const fill& ended) noexcept {
    if (const auto listed = fills_.find(ended.key_); listed != fills_.end() && listed->second.get() == &ended) {
        fills_.erase(listed);
    }
}

void db_query_cache_state::unlist_fills(std::span<const std::pmr::string> keys) noexcept {
    for (const auto& removed : keys) {
        fills_.erase(std::string_view(removed));
    }
}

void db_query_cache_state::unlist_all_fills() noexcept {
    fills_.clear();
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
//
// A miss joins the fill already running for the same key on this worker, if
// any (Pingora read_through), and waits for it within the caller's own
// deadline and stop token: the leader's published result answers it like a
// hit, and a leader that ended without one sends it back to the cache, where
// the first to miss again leads the next fill. Each worker fills a key at most
// once at a time; other workers and processes fill it independently.
task<db_rows> db_query_cache_state::query(std::pmr::string key, std::chrono::milliseconds duration,
    db_cache_query database, operation_options options, operation_timeout deadline) {
    stop_group cancellation{options.stop_token_, stop_.token()};
    options.stop_token_ = cancellation.token();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::optional<std::pmr::string> bytes_value;
        try {
            bytes_value = co_await redis_.with_options(db_cache_options(options, deadline)).get(key);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (bytes_value.has_value()) {
            if (auto rows = decode_cached_rows(std::move(*bytes_value), resource_, deadline, ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        const auto running = fills_.find(key);
        if (running == fills_.end()) {
            break;
        }
        const auto joined = running->second;
        joined->awaited_ = true;
        timer_sleep_result woken{};
        {
            stop_group wake{options.stop_token_, joined->done_.token()};
            woken = co_await sleep_until(
                worker_, deadline.deadline().value_or(std::chrono::steady_clock::time_point::max()), wake.token());
        }
        if (options.stop_token_.stop_requested()) {
            require_open();
            throw db_error(db_error::code_type::cancelled, std::nullopt, "database query cache operation cancelled");
        }
        if (joined->result_.has_value()) {
            if (auto rows = decode_cached_rows(std::pmr::string(*joined->result_, resource_), resource_, deadline,
                    ignore_errors_)) {
                co_return std::move(*rows);
            }
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
        // Cut short with the fill still running: the worker is stopping.
        if (woken == timer_sleep_result::stop_requested && !joined->done_.stop_requested()) {
            throw db_error(db_error::code_type::closing, std::nullopt, "database query cache is closing");
        }
    }

    auto listed = std::allocate_shared<fill>(std::pmr::polymorphic_allocator<fill>(resource_), key, resource_);
    fills_.emplace(std::string_view(listed->key_), listed);
    fill_lead lead(*this, std::move(listed));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    auto rows = co_await std::move(database)(db_remaining_options(options, deadline));
    if (deadline.expired()) {
        throw_db_cache_timeout();
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    auto& filled = lead.get();
    const bool store = elapsed < duration;
    if (!store && !filled.awaited_) {
        co_return rows;
    }
    auto encoded_bytes = encode_db_cache_rows(rows, resource_);
    // An entry whose GET reply would exceed the store's reply limit would fail
    // every hit and cost a Redis connection each time; like Pingora's
    // ResponseTooLarge, the result stays valid but is not cached.
    const bool fits = !max_entry_reply_bytes_.has_value() ||
                      resp_bulk_serialized_size(encoded_bytes.size()) <= *max_entry_reply_bytes_;
    std::optional<task<redis_set_result>> write;
    if (store && fits) {
        try {
            // SET owns its arguments before returning the cold operation.
            write.emplace(redis_.with_options(db_cache_options(options, deadline))
                    .set(key, encoded_bytes, {.expiration_ = redis_set_expiration::expires_after(duration - elapsed)}));
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
    }
    // Waiters take the result from memory; neither the key nor the serialized
    // rows are retained here while the SET waits on IO.
    if (filled.awaited_) {
        filled.result_ = std::move(encoded_bytes);
    }
    std::pmr::string(encoded_bytes.get_allocator().resource()).swap(encoded_bytes);
    std::pmr::string(key.get_allocator().resource()).swap(key);
    lead.finish();
    if (write.has_value()) {
        try {
            (void)co_await std::move(*write);
        } catch (const redis_error& error) {
            rethrow_or_timeout_redis_cache_error(error, deadline, ignore_errors_);
        }
        if (deadline.expired()) {
            throw_db_cache_timeout();
        }
    }
    co_return rows;
}

// A cache id names every result shape a repository builder derived from it.
task<void> db_query_cache_state::remove(
    std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options) {
    require_open();
    for (const auto id : ids) {
        if (id.empty()) {
            throw std::invalid_argument("cache identifier must not be empty");
        }
    }
    std::pmr::vector<std::pmr::string> keys(resource_);
    keys.reserve(ids.size() * db_cache_shapes.size());
    for (const auto id : ids) {
        append_db_cache_id_keys(keys, prefix_, id);
    }
    unlist_fills(keys);
    return remove_keys(redis_, std::move(keys), io_metrics, std::move(options), stop_.token());
}

task<void> db_query_cache_state::clear(request_io_metrics* io_metrics, operation_options options) {
    require_open();
    std::pmr::string pattern(resource_);
    pattern.reserve(prefix_.size() + 1);
    pattern.append(prefix_);
    pattern.push_back('*');
    unlist_all_fills();
    return clear_keys(redis_, std::move(pattern), io_metrics, std::move(options), stop_.token());
}

}  // namespace ruvia::detail

namespace ruvia {
db_query_result_cache::db_query_result_cache(detail::db_query_cache_state& state_value, operation_scope& scope,
    detail::request_io_metrics* io_metrics, operation_options options) noexcept
    : state_(&state_value),
      io_metrics_(io_metrics),
      options_(std::move(options)),
      registration_(scope, this, &db_query_result_cache::expire_capability) {}

db_query_result_cache::db_query_result_cache(const db_query_result_cache& other) noexcept
    : state_(other.state_),
      io_metrics_(other.io_metrics_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void db_query_result_cache::expire_capability(void* target) noexcept {
    auto& cache = *static_cast<db_query_result_cache*>(target);
    cache.state_ = nullptr;
    cache.io_metrics_ = nullptr;
    cache.options_ = {};
}
task<void> db_query_result_cache::remove(std::span<const std::string_view> ids) const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->remove(ids, io_metrics_, options_));
}
task<void> db_query_result_cache::clear() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), state_->clear(io_metrics_, options_));
}
}  // namespace ruvia
