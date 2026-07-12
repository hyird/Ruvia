#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/Hex.h"
#include "ruvia/web/detail/json/JsonLex.h"

namespace ruvia::detail {

// Read exactly four hex digits as a UTF-16 code unit. Single owner of \uXXXX
// digit decoding for both the validation scan (parseJsonStringRaw) and the
// decode pass (decodeJsonString).
[[nodiscard]] inline bool readJsonHex4(std::string_view input, std::uint32_t& value) noexcept {
    if (input.size() < 4) {
        return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto hex = decodeHexNibble(input[i]);
        if (hex < 0) {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(hex);
    }
    return true;
}

// Length (1-4) of the well-formed UTF-8 sequence beginning at input[i] (whose
// lead byte is >= 0x80), or 0 if the bytes there are not valid UTF-8. Enforces the
// Unicode 3.9 Table 3-7 constraints: no overlong forms (C0/C1, E0 80-9F, F0 80-8F),
// no UTF-16 surrogate code points (ED A0-BF), nothing above U+10FFFF (F4 90-.., F5+),
// and no stray/truncated continuation bytes. RFC 8259 §8.1 requires JSON to be UTF-8,
// so a string carrying anything else is rejected rather than passed through verbatim.
[[nodiscard]] inline std::size_t jsonUtf8SequenceLength(std::string_view input, std::size_t i) noexcept {
    const auto isCont = [](unsigned char b) noexcept { return (b & 0xC0U) == 0x80U; };
    const auto b0 = static_cast<unsigned char>(input[i]);
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return (i + 1 < input.size() && isCont(static_cast<unsigned char>(input[i + 1]))) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (i + 2 >= input.size()) {
            return 0;
        }
        const auto b1 = static_cast<unsigned char>(input[i + 1]);
        const auto b2 = static_cast<unsigned char>(input[i + 2]);
        if (!isCont(b2)) {
            return 0;
        }
        if (b0 == 0xE0 ? (b1 < 0xA0 || b1 > 0xBF)
                       : b0 == 0xED ? (b1 < 0x80 || b1 > 0x9F) : !isCont(b1)) {
            return 0;
        }
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (i + 3 >= input.size()) {
            return 0;
        }
        const auto b1 = static_cast<unsigned char>(input[i + 1]);
        const auto b2 = static_cast<unsigned char>(input[i + 2]);
        const auto b3 = static_cast<unsigned char>(input[i + 3]);
        if (!isCont(b2) || !isCont(b3)) {
            return 0;
        }
        if (b0 == 0xF0 ? (b1 < 0x90 || b1 > 0xBF)
                       : b0 == 0xF4 ? (b1 < 0x80 || b1 > 0x8F) : !isCont(b1)) {
            return 0;
        }
        return 4;
    }
    return 0;  // 0x80-0xC1 (bare continuation / overlong lead) or 0xF5-0xFF
}

[[nodiscard]] inline bool parseJsonStringRaw(
    std::string_view& input,
    std::string_view& value,
    bool& escaped) noexcept {
    skipJsonWhitespace(input);
    if (input.empty() || input.front() != '"') {
        return false;
    }
    input.remove_prefix(1);

    escaped = false;
    const char* const begin = input.data();
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '\\') {
            escaped = true;
            if (i + 1 >= input.size()) {
                return false;
            }
            const char escape = input[i + 1];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
                escape == 'n' || escape == 'r' || escape == 't') {
                ++i;
                continue;
            }
            if (escape == 'u') {
                std::uint32_t ignored = 0;
                if (i + 5 >= input.size() || !readJsonHex4(input.substr(i + 2), ignored)) {
                    return false;
                }
                i += 5;
                continue;
            }
            return false;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            return false;
        }
        if (c == '"') {
            value = std::string_view(begin, i);
            input.remove_prefix(i + 1);
            return true;
        }
        if (uc >= 0x80) {
            const auto length = jsonUtf8SequenceLength(input, i);
            if (length == 0) {
                return false;
            }
            i += length - 1;  // the loop's ++i steps past the final byte
        }
    }

    return false;
}

[[nodiscard]] inline bool parseJsonStringView(std::string_view& input, std::string_view& value) noexcept {
    bool escaped = false;
    if (!parseJsonStringRaw(input, value, escaped)) {
        return false;
    }
    return !escaped;
}

template <typename OutputT>
void appendUtf8(OutputT& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Returns the complete decoded string or no value for malformed escape/UTF-8
// input. Mutable caller storage is intentionally not accepted: a failure must
// never expose the prefix produced before the malformed byte sequence.
[[nodiscard]] inline std::optional<std::pmr::string> decodeJsonString(
    std::string_view input,
    std::pmr::memory_resource* resource) {
    std::pmr::string output(pmrResourceOrDefault(resource));
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c != '\\') {
            const auto uc = static_cast<unsigned char>(c);
            if (uc < 0x20) {
                return std::nullopt;
            }
            if (uc >= 0x80) {
                const auto length = jsonUtf8SequenceLength(input, i);
                if (length == 0) {
                    return std::nullopt;
                }
                for (std::size_t k = 0; k < length; ++k) {
                    output.push_back(input[i + k]);
                }
                i += length - 1;  // the loop's ++i steps past the final byte
                continue;
            }
            output.push_back(c);
            continue;
        }

        if (i + 1 >= input.size()) {
            return std::nullopt;
        }
        const char escape = input[++i];
        switch (escape) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escape);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codePoint = 0;
                if (!readJsonHex4(input.substr(i + 1), codePoint)) {
                    return std::nullopt;
                }
                i += 4;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (i + 6 >= input.size() || input[i + 1] != '\\' || input[i + 2] != 'u') {
                        return std::nullopt;
                    }
                    std::uint32_t low = 0;
                    if (!readJsonHex4(input.substr(i + 3), low) || low < 0xDC00 || low > 0xDFFF) {
                        return std::nullopt;
                    }
                    i += 6;
                    codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (low - 0xDC00));
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return std::nullopt;
                }
                appendUtf8(output, codePoint);
                break;
            }
            default:
                return std::nullopt;
        }
    }
    return output;
}

[[nodiscard]] inline bool skipJsonString(std::string_view& input) noexcept {
    std::string_view ignored;
    bool escaped = false;
    return parseJsonStringRaw(input, ignored, escaped);
}

}  // namespace ruvia::detail
