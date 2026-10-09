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

namespace ruvia {

[[nodiscard]] inline std::size_t unsigned_decimal_size(std::uint64_t value) noexcept {
    std::size_t size = 1;
    while (value >= 10) {
        value /= 10;
        ++size;
    }
    return size;
}

template <typename number_t_type>
inline void append_formatted_number(
    std::pmr::string& output, number_t_type value, const char* error_message) {
    std::array<char, 64> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::logic_error(error_message);
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

template <typename number_t_type>
inline void append_formatted_finite_number(std::pmr::string& output, number_t_type value,
    const char* finite_error_message, const char* format_error_message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(finite_error_message);
    }
    std::array<char, 64> buffer;
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc{}) {
        throw std::invalid_argument(format_error_message);
    }
    output.append(buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

}  // namespace ruvia
