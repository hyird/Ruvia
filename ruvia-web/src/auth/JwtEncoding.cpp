#include "ruvia/web/detail/auth/JwtInternal.h"

#include "ruvia/core/detail/Base64Url.h"

#include <cstdint>
#include <stdexcept>

namespace ruvia::detail {

std::pmr::string jwtBase64UrlEncode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmrResourceOrDefault(resource));
    out.reserve((input.size() * 4 + 2) / 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kBase64UrlAlphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kBase64UrlAlphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    return out;
}

std::pmr::string jwtBase64UrlDecode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmrResourceOrDefault(resource));
    out.reserve(input.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        const auto value = decodeBase64UrlChar(ch);
        if (value < 0) {
            throw std::invalid_argument("JWT base64url value is invalid");
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    // A length of 1 (mod 4) cannot encode any byte group; reject it rather than
    // silently dropping the stray 6 bits.
    if (bits >= 6) {
        throw std::invalid_argument("JWT base64url has invalid length");
    }
    // Require canonical encoding: the trailing unused bits of the final char must
    // be zero, so a given input maps to exactly one byte string.
    if (bits > 0 && (buffer & ((std::uint32_t{1} << bits) - 1)) != 0) {
        throw std::invalid_argument("JWT base64url is not canonical");
    }
    return out;
}

}  // namespace ruvia::detail
