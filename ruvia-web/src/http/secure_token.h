#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace ruvia::detail {

// Lowercase hex characters of a token: 24 random bytes, 192 bits.
inline constexpr std::size_t secure_token_size = 48;

// Fills `buffer` from OpenSSL's CSPRNG and returns it as a view. A generator
// failure is a server fault, not a request outcome: it throws
// std::runtime_error, which error dispatch answers with a generic 500 and
// reports.
[[nodiscard]] std::string_view generate_secure_token(std::span<char, secure_token_size> buffer);

}  // namespace ruvia::detail
