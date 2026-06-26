#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/db/DbTypes.h"

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
    explicit QueryResult(std::pmr::memory_resource* resource = nullptr);

    QueryResult(const QueryResult&) = delete;
    QueryResult& operator=(const QueryResult&) = delete;
    QueryResult(QueryResult&& other) noexcept;
    QueryResult& operator=(QueryResult&& other);
    ~QueryResult();

    [[nodiscard]] std::span<const DbRow> rows() const noexcept;
    [[nodiscard]] std::uint64_t affectedRows() const noexcept;
    [[nodiscard]] std::uint64_t lastInsertId() const noexcept;

private:
    friend class detail::MariaDbPool;
    friend class DbHandle;
    friend class DbTransaction;

    std::pmr::vector<DbRow> rows_;
    std::pmr::vector<DbField> fields_;
    std::uint64_t affectedRows_{0};
    std::uint64_t lastInsertId_{0};
    const QueryResult* mounted_{nullptr};
    st_mysql_res* rawResult_{nullptr};
    void (*releaseRawResult_)(st_mysql_res*) noexcept{nullptr};
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

    DbStreamResult(
        detail::MariaDbPool& client,
        std::size_t slot,
        void* result,
        std::pmr::memory_resource* resource) noexcept;
    void reset() noexcept;
    void release() noexcept;

    detail::MariaDbPool* client_{nullptr};
    std::size_t slot_{0};
    void* result_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    bool active_{false};
};

}  // namespace ruvia
