#pragma once

#include "ruvia/web/db/DbTransaction.h"
#include "ruvia/web/detail/db/DbBackend.h"

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

    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params = {}) const;
    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params = {}) const;
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    Task<DbStreamResult> queryStream(std::string_view sql, std::span<const DbValue> params = {}) const;
    Task<DbStreamResult> queryStream(std::string_view sql, std::initializer_list<DbValue> params) const = delete;
    Task<DbTransaction> beginTransaction() const;

private:
    friend class detail::DbRegistry;

    DbHandle(
        detail::DbPoolRef client,
        std::pmr::memory_resource* resource) noexcept;
    Task<QueryResult> executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) const;
    Task<DbStreamResult> queryStreamPrepared(std::pmr::string sql, std::pmr::vector<DbValue> params) const;

    detail::DbPoolRef client_;
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
