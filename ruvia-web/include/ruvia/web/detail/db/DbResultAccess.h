#pragma once

#include "ruvia/web/db/DbRows.h"

#include <exception>
#include <memory_resource>
#include <string_view>

namespace ruvia::detail {

// The public result objects stay backend-neutral. Concrete drivers populate
// them through this single internal access point instead of accumulating one
// friend declaration per driver.
struct DbResultAccess final {
    [[nodiscard]] static DbRows makeResult(std::pmr::memory_resource* resource) {
        return DbRows(resource);
    }

    [[nodiscard]] static constexpr DbExecResult makeExecResult(std::uint64_t affectedRows, std::optional<std::uint64_t> lastInsertId = std::nullopt) noexcept {
        return DbExecResult(affectedRows, lastInsertId);
    }

    [[nodiscard]] static std::pmr::vector<DbRow>& rows(DbRows& result) noexcept {
        return result.rows_;
    }

    [[nodiscard]] static std::pmr::vector<DbField>& fields(DbRows& result) noexcept {
        return result.fields_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& columnNames(DbRows& result) noexcept {
        return result.columnNames_;
    }

    static void ownRawResult(DbRows& result, void* raw, void (*release)(void*) noexcept) noexcept {
        if (raw == nullptr || release == nullptr || std::holds_alternative<DbRows::OwnedRawResult>(result.rawResult_)) {
            std::terminate();
        }
        result.rawResult_.template emplace<DbRows::OwnedRawResult>(raw, release);
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

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& ownedColumnNames(DbRow& row) noexcept {
        return row.ownedColumnNames();
    }

    [[nodiscard]] static DbRow borrowedRow(
        const DbField* fields,
        std::size_t size,
        const std::pmr::string* columnNames,
        std::size_t columnCount,
        std::pmr::memory_resource* resource) {
        return DbRow(fields, size, columnNames, columnCount, resource);
    }
};

}  // namespace ruvia::detail
