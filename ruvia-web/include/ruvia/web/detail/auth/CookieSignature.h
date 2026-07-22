#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::detail {

// base64(HMAC-SHA256) — fixed width, standard alphabet with padding.
inline constexpr std::size_t kCookieSignatureSize = 44;

// Writes exactly kCookieSignatureSize characters to `output`. The cookie `name`
// is bound into the MAC (length-framed together with `value`) so a value signed
// for one cookie is not a valid signature for another under the same secret.
// Throws std::invalid_argument on an empty secret.
void writeCookieSignature(
    char* output, std::string_view secret, std::string_view name, std::string_view value);

// Constant-time comparison; signature strings are attacker-controlled.
[[nodiscard]] bool cookieSignatureEquals(std::string_view left, std::string_view right) noexcept;

}  // namespace ruvia::detail
