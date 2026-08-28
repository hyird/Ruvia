#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ruvia::detail {

[[nodiscard]] inline std::size_t httpUnsignedDecimalSize(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

template <typename NumberT>
inline void appendHttpFormattedNumber(std::pmr::string& output, NumberT value, const char* errorMessage) {
    std::array<char, 64> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error(errorMessage);
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

template <typename NumberT>
inline void appendHttpFormattedFiniteNumber(std::pmr::string& output, NumberT value, const char* finiteErrorMessage, const char* formatErrorMessage) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(finiteErrorMessage);
    }
    std::array<char, 64> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument(formatErrorMessage);
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

}  // namespace ruvia::detail
