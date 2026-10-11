#pragma once

#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/db/db_types.h"

#include "db/db_query_execution.h"

// Query-result cache encoding and keys (Redis builds only). The cache store
// itself and its failure policy live in db_query_result_cache.cpp.

namespace ruvia::detail {

struct db_config_storage;

[[nodiscard]] std::pmr::string db_cache_scope(std::string_view name_space, std::string_view alias,
    const db_config_storage& config, std::pmr::memory_resource* resource);

[[nodiscard]] std::pmr::string encode_db_cache_rows(const db_rows& rows, std::pmr::memory_resource* resource);
[[nodiscard]] db_rows decode_db_cache_rows(std::string_view bytes_value, std::pmr::memory_resource* resource);
[[nodiscard]] std::pmr::string db_cache_key(std::string_view name_space, std::string_view id, std::string_view sql,
    std::span<const db_value> params, db_driver driver, std::pmr::memory_resource* resource);
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
    static task<db_rows> execute_owned(db_query_backend backend, std::pmr::string sql,
        std::pmr::vector<db_value> params, operation_options options);

    db_query_backend backend_;
    std::pmr::string sql_;
    std::pmr::vector<db_value> params_;
};

}  // namespace ruvia::detail
