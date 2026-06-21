#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ruvia::detail {

template <typename NumberT>
inline void appendFormattedNumber(
    std::pmr::string& output,
    NumberT value,
    const char* errorMessage) {
    std::array<char, 64> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error(errorMessage);
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

template <typename NumberT>
inline void appendFormattedFiniteNumber(
    std::pmr::string& output,
    NumberT value,
    const char* finiteErrorMessage,
    const char* formatErrorMessage) {
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
