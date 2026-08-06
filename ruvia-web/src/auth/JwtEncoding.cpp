#include "ruvia/web/detail/auth/JwtPrimitives.h"

#include "ruvia/core/detail/util/Base64Url.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {

namespace {

[[nodiscard]] std::size_t jwtBase64UrlEncodedSize(std::size_t inputSize) {
    constexpr auto kMax = std::numeric_limits<std::size_t>::max();
    const auto fullGroups = inputSize / 3;
    const auto remainder = inputSize % 3;
    if (fullGroups > kMax / 4) {
        throw std::length_error("JWT base64url output is too large");
    }

    const auto fullGroupSize = fullGroups * 4;
    if (remainder == 0) {
        return fullGroupSize;
    }
    if (fullGroupSize > kMax - 4) {
        throw std::length_error("JWT base64url output is too large");
    }
    return fullGroupSize + 4;
}

[[nodiscard]] constexpr std::size_t jwtBase64UrlDecodedSize(std::size_t inputSize) noexcept {
    const auto fullGroups = inputSize / 4;
    const auto remainder = inputSize % 4;
    // fullGroups * 3 cannot overflow: fullGroups <= max(size_t) / 4.
    return fullGroups * 3 + (remainder == 0 ? 0 : remainder - 1);
}

}  // namespace

std::pmr::string jwtBase64UrlEncode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmrResourceOrDefault(resource));
    out.reserve(jwtBase64UrlEncodedSize(input.size()));
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
    out.reserve(jwtBase64UrlDecodedSize(input.size()));
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
