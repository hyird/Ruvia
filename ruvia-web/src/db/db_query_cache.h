#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_query_execution.h"

// Query-result cache encoding and keys (Redis builds only). The cache store
// itself and its failure policy live in db_query_result_cache.cpp.

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

struct db_config_storage;

[[nodiscard]] std::pmr::string db_cache_scope(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource);
[[nodiscard]] db_rows decode_db_cache_rows(std::string_view bytes_value, std::pmr::memory_resource* resource);
// An id-keyed entry: the caller's id and the result shape are separate key parts.
[[nodiscard]] std::pmr::string db_cache_id_key(std::string_view name_space, std::string_view id, db_cache_shape shape,
    std::pmr::memory_resource* resource);
// Keyed by id and shape when the query names an id, otherwise by its SQL and parameters.
[[nodiscard]] std::pmr::string db_cache_key(std::string_view name_space, std::string_view id, db_cache_shape shape,
    std::string_view sql, std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string db_cache_prefix(std::string_view name_space, std::pmr::memory_resource* resource);

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

}  // namespace ruvia::detail
