#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/Hex.h"
#include "ruvia/http/detail/json/JsonLex.h"

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
        if (static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
        if (c == '"') {
            value = std::string_view(begin, i);
            input.remove_prefix(i + 1);
            return true;
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

template <typename OutputT>
[[nodiscard]] bool decodeJsonString(std::string_view input, OutputT& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            output.push_back(c);
            continue;
        }

        if (i + 1 >= input.size()) {
            return false;
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
                    return false;
                }
                i += 4;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (i + 6 >= input.size() || input[i + 1] != '\\' || input[i + 2] != 'u') {
                        return false;
                    }
                    std::uint32_t low = 0;
                    if (!readJsonHex4(input.substr(i + 3), low) || low < 0xDC00 || low > 0xDFFF) {
                        return false;
                    }
                    i += 6;
                    codePoint = 0x10000 + (((codePoint - 0xD800) << 10) | (low - 0xDC00));
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    return false;
                }
                appendUtf8(output, codePoint);
                break;
            }
            default:
                return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool skipJsonString(std::string_view& input) noexcept {
    std::string_view ignored;
    bool escaped = false;
    return parseJsonStringRaw(input, ignored, escaped);
}

}  // namespace ruvia::detail
