#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>

#include "binary_text_encoding.h"
#include "crypto.h"

namespace ruvia::detail {

using ::ruvia::cookie_signature_size;
using ::ruvia::sign_cookie_value;
using ::ruvia::verify_signed_cookie_value;
using ::ruvia::write_cookie_signature;

// Lowercase hex characters of a token: 24 random bytes, 192 bits.
inline constexpr std::size_t secure_token_size = 48;

[[nodiscard]] inline std::string_view generate_secure_token(std::span<char, secure_token_size> buffer) {
    std::array<std::byte, secure_token_size / 2> raw{};
    if (!fill_random_bytes(raw)) {
        throw std::runtime_error("secure random generation failed");
    }
    (void)encode_lower_hex(buffer, raw);
    return std::string_view(buffer.data(), buffer.size());
}

}  // namespace ruvia::detail
