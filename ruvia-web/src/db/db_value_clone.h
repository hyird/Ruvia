#pragma once

#include <memory_resource>
#include <span>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {

[[nodiscard]] inline db_value clone_db_value_for_resource(
    const db_value& value, std::pmr::memory_resource* resolved_resource) {
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            return db_value(nullptr);
        case db_value_type::string:
            return db_value_access::owned_string(
                std::pmr::string(db_value_access::text(value), resolved_resource));
        case db_value_type::signed_value:
            return db_value(db_value_access::signed_value(value));
        case db_value_type::unsigned_value:
            return db_value(db_value_access::unsigned_value(value));
        case db_value_type::double_value:
            return db_value(db_value_access::get_double_value(value));
        case db_value_type::bool_value:
            return db_value(db_value_access::get_bool_value(value));
    }
    return db_value(nullptr);
}

[[nodiscard]] inline std::pmr::vector<db_value> clone_db_values(
    std::span<const db_value> values, std::pmr::memory_resource* resource) {
    auto* resolved = pmr_resource_or_default(resource);
    std::pmr::vector<db_value> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(clone_db_value_for_resource(value, resolved));
    }
    return output;
}

}  // namespace ruvia::detail
