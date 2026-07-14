#pragma once

#include "ruvia/web/db/DbQueryResult.h"

#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia {

class DbTransaction final {
public:
    DbTransaction(const DbTransaction&) = delete;
    DbTransaction& operator=(const DbTransaction&) = delete;
    DbTransaction(DbTransaction&& other) noexcept;
    DbTransaction& operator=(DbTransaction&& other) noexcept;
    ~DbTransaction();

    [[nodiscard]] bool active() const noexcept;
    Task<QueryResult> query(std::string_view sql, std::span<const DbValue> params = {});
    Task<QueryResult> query(std::string_view sql, std::initializer_list<DbValue> params) = delete;
    Task<QueryResult> execute(std::string_view sql, std::span<const DbValue> params = {});
    Task<QueryResult> execute(std::string_view sql, std::initializer_list<DbValue> params) = delete;
    Task<void> commit();
    Task<void> rollback();

private:
    friend class detail::MariaDbPool;

    DbTransaction(
        detail::MariaDbPool& client,
        std::size_t slot,
        std::pmr::memory_resource* resource,
        RequestMemory* requestMemory = nullptr) noexcept;
    Task<QueryResult> executePrepared(std::pmr::string sql, std::pmr::vector<DbValue> params);
    [[nodiscard]] QueryResult mountResult(QueryResult result) const;
    void reset() noexcept;

    detail::MariaDbPool* client_{nullptr};
    std::size_t slot_{0};
    std::pmr::memory_resource* resource_{nullptr};
    RequestMemory* requestMemory_{nullptr};
    bool active_{false};
};

}  // namespace ruvia
