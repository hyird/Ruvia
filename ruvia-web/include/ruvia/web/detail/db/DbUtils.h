#pragma once

#include "ruvia/web/detail/db/DbValueAccess.h"
#include "ruvia/web/db/Db.h"

#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/util/HttpNumberFormat.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

inline void appendDbNumber(std::pmr::string& output, std::int64_t value) {
    appendHttpFormattedNumber(output, value, "failed to format signed database value");
}

inline void appendDbNumber(std::pmr::string& output, std::uint64_t value) {
    appendHttpFormattedNumber(output, value, "failed to format unsigned database value");
}

inline void appendDbNumber(std::pmr::string& output, double value) {
    // std::to_chars renders inf/nan as the literal words "inf"/"nan", which are
    // not valid SQL numeric literals and would be spliced unquoted into the
    // statement. Reject them up front with a clear error instead of letting the
    // server fail on malformed SQL.
    appendHttpFormattedFiniteNumber(output, value, "database double value must be finite",
        "database double value cannot be formatted");
}

[[nodiscard]] inline DbValue cloneDbValueForResource(
    const DbValue& value, std::pmr::memory_resource* resolvedResource) {
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            return DbValue(nullptr);
        case DbValueType::kString:
            return DbValueAccess::ownedString(
                std::pmr::string(DbValueAccess::text(value), resolvedResource));
        case DbValueType::kSigned:
            return DbValue(DbValueAccess::signedValue(value));
        case DbValueType::kUnsigned:
            return DbValue(DbValueAccess::unsignedValue(value));
        case DbValueType::kDouble:
            return DbValue(DbValueAccess::doubleValue(value));
        case DbValueType::kBool:
            return DbValue(DbValueAccess::boolValue(value));
    }
    return DbValue(nullptr);
}

[[nodiscard]] inline std::pmr::vector<DbValue> cloneDbValues(
    std::span<const DbValue> values, std::pmr::memory_resource* resource) {
    auto* resolved = pmrResourceOrDefault(resource);
    std::pmr::vector<DbValue> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(cloneDbValueForResource(value, resolved));
    }
    return output;
}

// Every pool operation dispatches on the configured backend; reaching the end
// means the build has no driver for it.
[[noreturn]] inline void throwUnavailableDbBackend() {
    throw std::logic_error("database backend is not available");
}

// DbPoolRef is a closed backend set. Keep its single checked dispatch here so
// handle, stream, transaction, and registry operations cannot drift into
// subtly different null or unavailable-backend behavior.
template <typename Visitor>
decltype(auto) visitDbPool(const DbPoolRef& pool, Visitor&& visitor) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<Visitor>(visitor)(**client);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<Visitor>(visitor)(**client);
    }
#endif
    throwUnavailableDbBackend();
}

// Destruction and immediate close paths cannot report an empty backend. They
// deliberately ignore it while preserving the same closed-set dispatch.
template <typename Visitor>
void visitDbPoolIfPresent(const DbPoolRef& pool, Visitor&& visitor) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<Visitor>(visitor)(**client);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<Visitor>(visitor)(**client);
    }
#endif
}

}  // namespace ruvia::detail
