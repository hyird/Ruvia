#include <cstdint>
#include <limits>
#include <stdexcept>

#include "ruvia/core/base64_url.h"

#include "auth/jwt_primitives.h"

namespace ruvia::detail {

namespace {

[[nodiscard]] std::size_t jwt_base64_url_encoded_size(std::size_t input_size) {
    constexpr auto max_value = std::numeric_limits<std::size_t>::max();
    const auto full_groups = input_size / 3;
    const auto remainder = input_size % 3;
    if (full_groups > max_value / 4) {
        throw std::length_error("JWT base64url output is too large");
    }

    const auto full_group_size = full_groups * 4;
    if (remainder == 0) {
        return full_group_size;
    }
    const auto suffix_size = remainder + 1;
    if (full_group_size > max_value - suffix_size) {
        throw std::length_error("JWT base64url output is too large");
    }
    return full_group_size + suffix_size;
}

[[nodiscard]] constexpr std::size_t jwt_base64_url_decoded_size(std::size_t input_size) noexcept {
    const auto full_groups = input_size / 4;
    const auto remainder = input_size % 4;
    // full_groups * 3 cannot overflow: full_groups <= max(size_t) / 4.
    return full_groups * 3 + (remainder == 0 ? 0 : remainder - 1);
}

}  // namespace

std::pmr::string jwt_base64_url_encode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmr_resource_or_default(resource));
    out.resize(jwt_base64_url_encoded_size(input.size()));
    std::size_t written = 0;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[written++] = ruvia::base64_url_alphabet[(buffer >> bits) & 0x3F];
        }
    }
    if (bits > 0) {
        out[written++] = ruvia::base64_url_alphabet[(buffer << (6 - bits)) & 0x3F];
    }
    out.resize(written);
    return out;
}

std::pmr::string jwt_base64_url_decode(std::string_view input, std::pmr::memory_resource* resource) {
    std::pmr::string out(pmr_resource_or_default(resource));
    out.resize(jwt_base64_url_decoded_size(input.size()));
    std::size_t written = 0;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto ch : input) {
        const auto value = ruvia::decode_base64_url_char(ch);
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
