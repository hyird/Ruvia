#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>

namespace ruvia::detail {

// Append the shortest round-trip decimal form of a finite floating value
// (float keeps its own shortest form). NaN and infinities throw
// std::invalid_argument(`nonfinite_message`) before `output` is touched;
// std::to_chars cannot fail for a finite value in 64 bytes.
template <std::floating_point value_type>
void append_finite_double(std::pmr::string& output, value_type value, const char* nonfinite_message) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(nonfinite_message);
    }
    std::array<char, 64> buffer;
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    output.append(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

}  // namespace ruvia::detail
