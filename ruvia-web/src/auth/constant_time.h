#pragma once

#include <string_view>

#include <openssl/crypto.h>

namespace ruvia::detail {

// Length-checked, data-independent (constant-time) byte comparison for secrets
// and authentication tags. It never
// early-outs on the first differing byte, so the time taken does not leak how
// far an attacker-supplied value matched the expected one. Unequal lengths are
// reported immediately; that is not secret since these values are fixed-length.
[[nodiscard]] inline bool constant_time_bytes_equal(
    std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace ruvia::detail
