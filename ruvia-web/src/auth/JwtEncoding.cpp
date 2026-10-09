#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ruvia/core/Base64Url.h"

#include "auth/JwtPrimitives.h"

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
    const auto suffixSize = remainder + 1;
    if (fullGroupSize > kMax - suffixSize) {
        throw std::length_error("JWT base64url output is too large");
    }
    return fullGroupSize + suffixSize;
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
    out.resize(jwtBase64UrlEncodedSize(input.size()));
    std::size_t written = 0;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[written++] = ruvia::kBase64UrlAlphabet[(buffer >> bits) & 0x3F];
        }
    }
    if (bits > 0) {
        out[written++] = ruvia::kBase64UrlAlphabet[(buffer << (6 - bits)) & 0x3F];
    }
    out.resize(written);
    return out;
}

std::pmr::string jwtBase64UrlDecode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmrResourceOrDefault(resource));
    out.resize(jwtBase64UrlDecodedSize(input.size()));
    std::size_t written = 0;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        const auto value = ruvia::decodeBase64UrlChar(ch);
        if (value < 0) {
            throw std::invalid_argument("JWT base64url value is invalid");
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[written++] = static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    // A length of 1 (mod 4) cannot encode any byte group.
    if (bits >= 6) {
        throw std::invalid_argument("JWT base64url has invalid length");
    }
    // Unused trailing bits must be zero for a canonical representation.
    if (bits > 0 && (buffer & ((std::uint32_t{1} << bits) - 1)) != 0) {
        throw std::invalid_argument("JWT base64url is not canonical");
    }
    out.resize(written);
    return out;
}

}  // namespace ruvia::detail
