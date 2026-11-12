#pragma once

#include <array>
#include <memory_resource>
#include <tuple>

#include "ruvia/web/detail/db/db_entity_codec.h"

namespace ruvia {

// Decoding owns every text/array element in the requested resource. SQL NULL
// requires an optional destination; malformed values keep db_conversion_error.
template <db_mapped_value value_type>
[[nodiscard]] value_type decode_db_field(const db_field& field, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = pmr_resource_or_default(resource);
    auto result = detail::entity_value_slot<value_type>::make_value(resolved);
    detail::decode_db_field(field, result, resolved);
    return result;
}

// A projection describes a subset of columns. Every declared column is required
// and must be unique in the result; unrelated result columns are ignored.
template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_type decode_db_row(const db_row& row, std::pmr::memory_resource* resource = nullptr) {
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    return decoder.decode(row, pmr_resource_or_default(resource));
}

template <typename entity_type>
    requires(sql_entity<entity_type> || sql_projection<entity_type>)
[[nodiscard]] entity_rows<entity_type> decode_db_rows(const db_rows& rows, std::pmr::memory_resource* resource = nullptr) {
    auto* resolved = pmr_resource_or_default(resource);
    auto result = detail::db_result_access::make_entity_rows<entity_type>(resolved, rows.size());
    std::array<bool, std::tuple_size_v<typename entity_type::columns_type>> selected;
    selected.fill(true);
    detail::db_entity_row_decoder<entity_type> decoder(selected);
    for (const auto& row : rows) {
        result.push_back(decoder.decode(row, resolved));
    }
    return result;
}

}  // namespace ruvia
