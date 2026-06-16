#pragma once

#include "ruvia/db/Db.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

[[nodiscard]] inline std::pmr::memory_resource* resolveDbResource(
    std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

inline void appendDbNumber(std::pmr::string& output, std::int64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format signed database value");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

inline void appendDbNumber(std::pmr::string& output, std::uint64_t value) {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error("failed to format unsigned database value");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

inline void appendDbNumber(std::pmr::string& output, double value) {
    std::array<char, 64> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument("database double value cannot be formatted");
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

[[nodiscard]] inline DbValue cloneDbValueForResource(
    const DbValue& value,
    std::pmr::memory_resource* resource) {
    switch (value.type()) {
        case DbValueType::kNull:
            return DbValue(nullptr);
        case DbValueType::kString:
            return DbValue(std::pmr::string(value.text(), resolveDbResource(resource)));
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
