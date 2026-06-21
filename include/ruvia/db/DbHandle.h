#pragma once

#include "ruvia/db/DbTransaction.h"

#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

class DbHandle final {
public:
    DbHandle(const DbHandle&) = default;
    DbHandle& operator=(const DbHandle&) = delete;

    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params) const;
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params) const;
    Task<DbStreamResult> queryStream(std::string_view sql, std::initializer_list<DbValue> params = {}) const;
    Task<DbStreamResult> queryStream(std::string_view sql, std::span<const DbValue> params) const;
    Task<DbTransaction> beginTransaction() const;

private:
    friend class detail::DbRegistry;

    DbHandle(
        detail::MariaDbPool& client,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) noexcept;
    Task<QueryResult> executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) const;
    Task<DbStreamResult> queryStreamPrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) const;
    [[nodiscard]] QueryResult mountResult(QueryResult result) const;

    detail::MariaDbPool& client_;
    std::pmr::memory_resource* resource_;
    RequestMemory* requestMemory_{nullptr};
};

}  // namespace ruvia
