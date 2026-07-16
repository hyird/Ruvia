#pragma once

#include "ruvia/web/detail/db/DbValueAccess.h"
#include "ruvia/web/db/Db.h"

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/detail/HttpNumberFormat.h"
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
    appendHttpFormattedFiniteNumber(
        output,
        value,
        "database double value must be finite",
        "database double value cannot be formatted");
}

[[nodiscard]] inline DbValue cloneDbValueForResource(
    const DbValue& value,
    std::pmr::memory_resource* resolvedResource) {
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            return DbValue(nullptr);
        case DbValueType::kString:
            return DbValueAccess::ownedString(std::pmr::string(
                DbValueAccess::text(value), resolvedResource));
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
    std::span<const DbValue> values,
    std::pmr::memory_resource* resource) {
    auto* resolved = pmrResourceOrDefault(resource);
    std::pmr::vector<DbValue> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(cloneDbValueForResource(value, resolved));
    }
    return output;
}

}  // namespace ruvia::detail
