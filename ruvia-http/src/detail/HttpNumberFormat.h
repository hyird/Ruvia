#pragma once

#include <array>
#include <charconv>
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
inline void appendHttpFormattedNumber(
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

}  // namespace ruvia::detail
