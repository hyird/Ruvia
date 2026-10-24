#pragma once

#include <span>

namespace ruvia::detail {

// Fills bytes from OpenSSL's CSPRNG. Returns false when the generator cannot
// produce output; callers translate that into their own failure contract.
[[nodiscard]] bool fill_random_bytes(std::span<unsigned char> bytes) noexcept;

}  // namespace ruvia::detail
