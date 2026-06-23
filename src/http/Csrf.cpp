#include "ruvia/http/Csrf.h"

#include <cstddef>

#include <openssl/rand.h>

namespace ruvia::detail {

std::string_view generateCsrfToken(std::span<char> buffer) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    constexpr std::size_t kRandomBytes = 24;  // 24 bytes -> 48 hex characters
    if (buffer.size() < kRandomBytes * 2) {
        return {};
    }
    unsigned char raw[kRandomBytes];
    if (RAND_bytes(raw, static_cast<int>(kRandomBytes)) != 1) {
        return {};
    }
    for (std::size_t i = 0; i < kRandomBytes; ++i) {
        buffer[i * 2] = kHex[(raw[i] >> 4) & 0x0F];
        buffer[i * 2 + 1] = kHex[raw[i] & 0x0F];
    }
    return std::string_view(buffer.data(), kRandomBytes * 2);
}

}  // namespace ruvia::detail
