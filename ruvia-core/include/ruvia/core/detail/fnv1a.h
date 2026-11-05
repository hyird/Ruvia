#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// 64-bit FNV-1a parameters (non-cryptographic; dispatch and fingerprint use only).
inline constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= fnv1a_prime;
    }
    return hash;
}

// Mixes an integer as its little-endian bytes so results do not depend on host order.
[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate_integer(std::uint64_t hash, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        hash ^= (value >> (i * 8U)) & 0xffU;
        hash *= fnv1a_prime;
    }
    return hash;
}

}  // namespace ruvia::detail
