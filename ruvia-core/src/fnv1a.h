#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia {

// 64-bit FNV-1a (non-cryptographic; dispatch, table and fingerprint use only).
inline constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;
inline constexpr std::uint64_t fnv1a_prime = 1099511628211ULL;

[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate_byte(std::uint64_t hash, unsigned char byte) noexcept {
    return (hash ^ byte) * fnv1a_prime;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate(std::uint64_t hash, std::string_view bytes) noexcept {
    for (const char byte : bytes) {
        hash = fnv1a_accumulate_byte(hash, static_cast<unsigned char>(byte));
    }
    return hash;
}

// Mixes an integer as its little-endian bytes so results do not depend on host order.
[[nodiscard]] constexpr std::uint64_t fnv1a_accumulate_integer(std::uint64_t hash, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        hash = fnv1a_accumulate_byte(hash, static_cast<unsigned char>(value >> (i * 8U)));
    }
    return hash;
}

}  // namespace ruvia
