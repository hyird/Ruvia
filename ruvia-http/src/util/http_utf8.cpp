#include "util/http_utf8.h"

#include <cstdint>

namespace ruvia::detail {

bool is_valid_utf8(std::string_view value) noexcept {
    std::uint32_t codepoint = 0;
    std::uint32_t remaining = 0;
    std::uint32_t min_value = 0;
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (remaining == 0) {
            if (byte <= 0x7F) {
                continue;
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                codepoint = byte & 0x1FU;
                remaining = 1;
                min_value = 0x80;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                codepoint = byte & 0x0FU;
                remaining = 2;
                min_value = 0x800;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                codepoint = byte & 0x07U;
                remaining = 3;
                min_value = 0x10000;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3FU);
            --remaining;
            if (remaining == 0 && (codepoint < min_value || codepoint > 0x10FFFF ||
                                      (codepoint >= 0xD800 && codepoint <= 0xDFFF))) {
                return false;
            }
        }
    }
    return remaining == 0;
}

}  // namespace ruvia::detail
