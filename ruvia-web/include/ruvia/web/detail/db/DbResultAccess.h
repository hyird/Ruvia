#pragma once

#include "ruvia/web/db/DbQueryResult.h"

#include <exception>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

// The public result objects stay backend-neutral. Concrete drivers populate
// them through this single internal access point instead of accumulating one
// friend declaration per driver.
struct DbResultAccess final {
    [[nodiscard]] static QueryResult makeResult(std::pmr::memory_resource* resource) {
        return QueryResult(resource);
    }

    static void setAffectedRows(QueryResult& result, std::uint64_t value) noexcept {
        result.affectedRows_ = value;
    }

    static void setLastInsertId(QueryResult& result, std::uint64_t value) noexcept {
        result.lastInsertId_ = value;
    }

    [[nodiscard]] static std::pmr::vector<DbRow>& rows(QueryResult& result) noexcept {
        return result.rows_;
    }

    [[nodiscard]] static std::pmr::vector<DbField>& fields(QueryResult& result) noexcept {
        return result.fields_;
    }

    static void ownRawResult(QueryResult& result, void* raw, void (*release)(void*) noexcept) noexcept {
        if (raw == nullptr || release == nullptr || std::holds_alternative<QueryResult::OwnedRawResult>(result.rawResult_)) {
            std::terminate();
        }
        result.rawResult_.template emplace<QueryResult::OwnedRawResult>(raw, release);
    }

    [[nodiscard]] static DbField nullField(std::pmr::memory_resource* resource) {
        return DbField(nullptr, resource);
    }

    [[nodiscard]] static DbField ownedField(std::string_view value, std::pmr::memory_resource* resource) {
        return DbField(value, resource);
    }

    [[nodiscard]] static DbField borrowedField(std::string_view value, std::pmr::memory_resource* resource) {
        return DbField::borrowed(value, resource);
    }

    [[nodiscard]] static DbRow ownedRow(std::pmr::memory_resource* resource) {
        return DbRow(resource);
    }

    [[nodiscard]] static std::pmr::vector<DbField>& ownedFields(DbRow& row) noexcept {
        return row.ownedFields();
    }

    [[nodiscard]] static DbRow borrowedRow(const DbField* fields, std::size_t size, std::pmr::memory_resource* resource) {
        return DbRow(fields, size, resource);
    }
};

}  // namespace ruvia::detail
