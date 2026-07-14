#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/web/db/DbTypes.h"
#include "ruvia/web/detail/db/DbBackend.h"
#include "ruvia/core/memory/PmrResource.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

struct st_mysql_res;

namespace ruvia {

class DbHandle;
class DbTransaction;

class QueryResult final {
public:
    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;
    QueryResult(QueryResult&& other) noexcept;
    QueryResult& operator=(QueryResult&& other);
    ~QueryResult();

    [[nodiscard]] std::span<const DbRow> rows() const noexcept;
    [[nodiscard]] std::uint64_t affectedRows() const noexcept;
    [[nodiscard]] std::uint64_t lastInsertId() const noexcept;

private:
    friend struct detail::DbResultAccess;
    friend class DbHandle;
    friend class DbTransaction;

    explicit QueryResult(std::pmr::memory_resource* resource = nullptr);
    QueryResult(detail::ResolvedPmrResourceTag, std::pmr::memory_resource* resource);

    std::pmr::vector<DbRow> rows_;
    std::pmr::vector<DbField> fields_;
    std::uint64_t affectedRows_{0};
    std::uint64_t lastInsertId_{0};
    const QueryResult* mounted_{nullptr};
    void* rawResult_{nullptr};
    void (*releaseRawResult_)(void*) noexcept{nullptr};
};

class DbStreamResult final {
public:
    DbStreamResult(const DbStreamResult&) = delete;
    DbStreamResult& operator=(const DbStreamResult&) = delete;
    DbStreamResult(DbStreamResult&& other) noexcept;
    DbStreamResult& operator=(DbStreamResult&& other) noexcept;
    ~DbStreamResult();

    [[nodiscard]] bool active() const noexcept;
    Task<std::optional<DbRow>> read();
    Task<void> close();

private:
    friend class detail::MariaDbPool;
    friend class detail::PostgreSqlPool;

    DbStreamResult(
        detail::DbPoolRef client,
        std::size_t slot,
        void* result,
        std::pmr::memory_resource* resource,
        bool active = true) noexcept;
    void reset() noexcept;
    void release() noexcept;

    detail::DbPoolRef client_{};
    std::size_t slot_{0};
    void* result_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    bool active_{false};
};

}  // namespace ruvia
