#include "JwtInternal.h"

#include <cstdint>
#include <stdexcept>

namespace ruvia::detail {
namespace {

constexpr std::string_view kBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

[[nodiscard]] int decodeBase64UrlChar(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') { return ch - 'A'; }
    if (ch >= 'a' && ch <= 'z') { return ch - 'a' + 26; }
    if (ch >= '0' && ch <= '9') { return ch - '0' + 52; }
    if (ch == '-') { return 62; }
    if (ch == '_') { return 63; }
    return -1;
}

}  // namespace

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
            out.push_back(kBase64Url[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kBase64Url[(buffer << (6 - bits)) & 0x3F]);
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
    return out;
}

}  // namespace ruvia::detail
