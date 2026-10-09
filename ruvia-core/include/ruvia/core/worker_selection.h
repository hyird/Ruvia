#pragma once

#include <cstdint>
#include <string_view>

namespace ruvia {

[[nodiscard]] constexpr std::uint64_t worker_selection_hash(std::string_view key) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch : key) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::worker_selection_hash;
}  // namespace ruvia::detail
