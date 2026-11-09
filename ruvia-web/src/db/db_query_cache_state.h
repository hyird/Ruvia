#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/db/db_query.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"
#include "ruvia/web/redis/redis_handle.h"

#include "db/db_config_storage.h"
#include "db/db_query_execution.h"
namespace ruvia::detail {

// Every result shape, so cache-id removal can name each entry of one id.
inline constexpr std::array db_cache_shapes{
    db_cache_shape::rows, db_cache_shape::one, db_cache_shape::exists, db_cache_shape::count};

// The key part naming each result shape, indexed by its value. An id-keyed
// entry stores the caller's id and this tag as separate key parts, so no user
// id can spell another id's shape.
inline constexpr std::array<std::string_view, db_cache_shapes.size()> db_cache_shape_tags{
    "rows", "one", "exists", "count"};

[[nodiscard]] constexpr std::string_view db_cache_shape_tag(db_cache_shape shape) noexcept {
    return db_cache_shape_tags[static_cast<std::size_t>(shape)];
}

// The key prefix of one alias's cache entries: everything that decides what a
// cached result means (namespace, alias, driver, server, database, user and
// TLS server name) digested into one key part.
[[nodiscard]] std::pmr::string db_cache_prefix(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource);
[[nodiscard]] db_rows decode_db_cache_rows(std::pmr::string bytes_value, std::pmr::memory_resource* resource);
// Key functions take the immutable prefix already built for this cache owner.
// Append all result shapes for one id, sharing the id digest.
void append_db_cache_id_keys(std::pmr::vector<std::pmr::string>& keys, std::string_view prefix, std::string_view id);
// Keyed by id and shape when the query names an id, otherwise by its SQL and parameters.
[[nodiscard]] std::pmr::string db_cache_key(std::string_view prefix, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource);

// The deferred database statement behind a cached step. On a cache hit it is
// destroyed without starting; on a miss it runs once on the plan's backend.
class db_cache_query final {
public:
    db_cache_query(const db_query_backend& backend, std::pmr::string sql, std::pmr::vector<db_value> params) noexcept
        : backend_(backend),
          sql_(std::move(sql)),
          params_(std::move(params)) {}

    db_cache_query(const db_cache_query&) = delete;
    db_cache_query& operator=(const db_cache_query&) = delete;
    db_cache_query(db_cache_query&&) noexcept = default;
    db_cache_query& operator=(db_cache_query&&) = delete;

    // The returned task owns the statement and the options it runs under.
    [[nodiscard]] task<db_rows> operator()(operation_options options) &&;

private:
    db_query_backend backend_;
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
};

// One database alias's Redis-backed query-result cache (Redis builds only).
// Worker-local: concurrent misses of one key on this worker share one fill.
class db_query_cache_state final {
public:
    // `prefix` is the alias's db_cache_prefix().
    db_query_cache_state(const redis_handle& redis, const db_cache_config_storage& config, std::pmr::string prefix,
        std::pmr::memory_resource* resource);
    ~db_query_cache_state();
    void close_now() noexcept;
    [[nodiscard]] std::optional<std::pmr::string> key(
        const db_query& query, const db_statement& statement, db_driver driver);
    // Serves the step from the cache, or runs `database` and stores its rows.
    [[nodiscard]] task<db_rows> wrap(std::optional<std::chrono::milliseconds> duration, std::pmr::string key,
        db_cache_query database, operation_options options, operation_timeout deadline);
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] task<void> remove(
        std::span<const std::string_view> ids, request_io_metrics* io_metrics, operation_options options);
    [[nodiscard]] task<void> clear(request_io_metrics* io_metrics, operation_options options);

private:
    // One read-through fill in flight: the caller that missed first (the
    // leader) runs the statement; later misses of the same full key wait for
    // its encoded result, or for its end without one, and then look again.
    // Shared, so a waiter still reads it after the leader's frame is gone.
    struct fill final {
        fill(std::string_view key, std::pmr::memory_resource* resource)
            : key_(key, resource) {}

        std::pmr::string key_;
        // Requested once the fill ends, with or without a result.
        stop_source done_;
        std::optional<std::pmr::string> result_;
        // A waiter joined, so the leader publishes its result in memory.
        bool awaited_{false};
    };
    using fill_pointer = std::shared_ptr<fill>;
    class fill_lead;

    void require_open() const;
    task<db_rows> query(std::pmr::string key, std::chrono::milliseconds duration, db_cache_query database,
        operation_options options, operation_timeout deadline);
    // Ends the fill if it is still the one listed for its key. Later misses
    // of that key no longer join it.
    void unlist_fill(const fill& ended) noexcept;
    // After remove() or clear(), a fill that started before must not answer
    // callers that arrive after: they read the database themselves.
    void unlist_fills(std::span<const std::pmr::string> keys) noexcept;
    void unlist_all_fills() noexcept;

    bool closed_{false};
    stop_source stop_;
    std::pmr::memory_resource* resource_;
    const worker_handle& worker_;
    std::chrono::milliseconds duration_;
    bool always_enabled_;
    bool ignore_errors_;
    // The store's reply limit: a GET of a larger entry fails on every hit, so
    // such an entry is never written.
    std::optional<std::size_t> max_entry_reply_bytes_;
    std::pmr::string prefix_;
    redis_handle redis_;
    // Keyed by views of each fill's own key.
    std::pmr::unordered_map<std::string_view, fill_pointer> fills_;
};

}  // namespace ruvia::detail
