#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] constexpr std::uint64_t workerSelectionHash(
    std::string_view key) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch : key) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace ruvia::detail
