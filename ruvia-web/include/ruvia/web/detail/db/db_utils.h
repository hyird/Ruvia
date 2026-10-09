#pragma once

#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/number_format.h"
#include "ruvia/web/db/db_rows.h"
#include "ruvia/web/detail/db/db_value_access.h"

namespace ruvia::detail {

inline void append_db_number(std::pmr::string& output, std::int64_t value) {
    append_formatted_number(output, value, "failed to format signed database value");
}

inline void append_db_number(std::pmr::string& output, std::uint64_t value) {
    append_formatted_number(output, value, "failed to format unsigned database value");
}

inline void append_db_number(std::pmr::string& output, double value) {
    // std::to_chars renders inf/nan as the literal words "inf"/"nan", which are
    // not valid SQL numeric literals and would be spliced unquoted into the
    // statement. Reject them up front with a clear error instead of letting the
    // server fail on malformed SQL.
    append_formatted_finite_number(output, value, "database double value must be finite",
        "database double value cannot be formatted");
}

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

// Every pool operation dispatches on the configured backend; reaching the end
// means the build has no driver for it.
[[noreturn]] inline void throw_unavailable_db_backend() {
    throw std::logic_error("database backend is not available");
}

[[nodiscard]] inline db_driver db_pool_driver([[maybe_unused]] const db_pool_ref_type& pool) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<mariadb_pool*>(&pool); client != nullptr && *client != nullptr) {
        return db_driver::mariadb;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<postgresql_pool*>(&pool); client != nullptr && *client != nullptr) {
        return db_driver::postgresql;
    }
#endif
    throw_unavailable_db_backend();
}

// db_pool_ref_type is a closed backend set. Keep its single checked dispatch here so
// handle, stream, transaction, and registry operations cannot drift into
// subtly different null or unavailable-backend behavior.
template <typename visitor_type>
decltype(auto) visit_db_pool([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<mariadb_pool*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<postgresql_pool*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
    throw_unavailable_db_backend();
}

// Destruction and immediate close paths cannot report an empty backend. They
// deliberately ignore it while preserving the same closed-set dispatch.
template <typename visitor_type>
void visit_db_pool_if_present([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<mariadb_pool*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<postgresql_pool*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
    }
#endif
}

}  // namespace ruvia::detail
