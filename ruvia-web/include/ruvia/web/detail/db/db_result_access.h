#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <string_view>

#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/entity_rows.h"

namespace ruvia {
template <typename entity_type>
class entity_rows;
}

namespace ruvia::detail {

// The public result objects stay backend-neutral. Concrete drivers populate
// them through this single internal access point instead of accumulating one
// friend declaration per driver.
struct db_result_access final {
    template <typename entity_type>
    [[nodiscard]] static entity_rows<entity_type> make_entity_rows(std::pmr::memory_resource* resource, std::size_t size) {
        entity_rows<entity_type> result(resource);
        result.reserve(size);
        return result;
    }

    [[nodiscard]] static db_rows make_result(std::pmr::memory_resource* resource) {
        return db_rows(resource);
    }

    [[nodiscard]] static constexpr db_exec_result make_exec_result(std::uint64_t affected_rows,
        std::optional<std::uint64_t> last_insert_id = std::nullopt) noexcept {
        return db_exec_result(affected_rows, last_insert_id);
    }

    [[nodiscard]] static std::pmr::vector<db_row>& rows(db_rows& result_value) noexcept {
        return result_value.rows_;
    }

    [[nodiscard]] static std::pmr::vector<db_field>& fields(db_rows& result_value) noexcept {
        return result_value.fields_;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& column_names(db_rows& result_value) noexcept {
        return result_value.column_names_;
    }

    [[nodiscard]] static std::span<const std::pmr::string> column_names(const db_row& row) noexcept {
        return row.column_names();
    }

    static void own_raw_result(db_rows& result_value, void* raw, void (*release)(void*) noexcept) noexcept {
        if (raw == nullptr || release == nullptr ||
            std::holds_alternative<db_rows::owned_raw_result_type>(result_value.raw_result_)) {
            std::terminate();
        }
        result_value.raw_result_.template emplace<db_rows::owned_raw_result_type>(raw, release);
    }

    [[nodiscard]] static db_field null_field(std::pmr::memory_resource* resource) {
        return db_field(nullptr, resource);
    }

    [[nodiscard]] static db_field owned_field(
        std::string_view value, std::pmr::memory_resource* resource) {
        return db_field(value, resource);
    }

    [[nodiscard]] static db_field borrowed_field(
        std::string_view value, std::pmr::memory_resource* resource) {
        return db_field::borrowed(value, resource);
    }

    [[nodiscard]] static db_row owned_row(std::pmr::memory_resource* resource) {
        return db_row(resource);
    }

    [[nodiscard]] static std::pmr::vector<db_field>& owned_fields(db_row& row) noexcept {
        return row.owned_fields();
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string>& owned_column_names(db_row& row) noexcept {
        return row.owned_column_names();
    }

    [[nodiscard]] static db_row borrowed_row(const db_field* fields_value, std::size_t size,
        const std::pmr::string* column_names, std::size_t column_count,
        std::pmr::memory_resource* resource) {
        return db_row(fields_value, size, column_names, column_count, resource);
    }
};

}  // namespace ruvia::detail
