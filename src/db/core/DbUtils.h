#pragma once

#include "ruvia/db/Db.h"

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/detail/NumberFormat.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* resolveDbResource(
    std::pmr::memory_resource* resource) noexcept {
    return pmrResourceOrDefault(resource);
}

inline void appendDbNumber(std::pmr::string& output, std::int64_t value) {
    appendFormattedNumber(output, value, "failed to format signed database value");
}

inline void appendDbNumber(std::pmr::string& output, std::uint64_t value) {
    appendFormattedNumber(output, value, "failed to format unsigned database value");
}

inline void appendDbNumber(std::pmr::string& output, double value) {
    // std::to_chars renders inf/nan as the literal words "inf"/"nan", which are
    // not valid SQL numeric literals and would be spliced unquoted into the
    // statement. Reject them up front with a clear error instead of letting the
    // server fail on malformed SQL.
    appendFormattedFiniteNumber(
        output,
        value,
        "database double value must be finite",
        "database double value cannot be formatted");
}

[[nodiscard]] inline DbValue cloneDbValueForResource(
    const DbValue& value,
    std::pmr::memory_resource* resolvedResource) {
    switch (value.type()) {
        case DbValueType::kNull:
            return DbValue(nullptr);
        case DbValueType::kString:
            return DbValue(std::pmr::string(value.text(), resolvedResource));
        case DbValueType::kSigned:
            return DbValue(value.signedValue());
        case DbValueType::kUnsigned:
            return DbValue(value.unsignedValue());
        case DbValueType::kDouble:
            return DbValue(value.doubleValue());
        case DbValueType::kBool:
            return DbValue(value.boolValue());
    }
    return DbValue(nullptr);
}

[[nodiscard]] inline std::pmr::vector<DbValue> cloneDbValues(
    std::span<const DbValue> values,
    std::pmr::memory_resource* resource) {
    auto* resolved = resolveDbResource(resource);
    std::pmr::vector<DbValue> output(resolved);
    output.reserve(values.size());
    for (const auto& value : values) {
        output.push_back(cloneDbValueForResource(value, resolved));
    }
    return output;
}

}  // namespace ruvia::detail
