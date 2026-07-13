#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

[[nodiscard]] inline char jsonHexDigit(std::uint8_t value) noexcept {
    return static_cast<char>(value < 10 ? ('0' + value) : ('A' + value - 10));
}

[[nodiscard]] inline bool jsonNeedsEscape(unsigned char value) noexcept {
    return value == '"' || value == '\\' || value < 0x20;
}

[[nodiscard]] inline std::size_t jsonStringSizeHint(std::string_view value) noexcept {
    std::size_t size = 2;
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                size += 2;
                break;
            default:
                size += c < 0x20 ? 6 : 1;
                break;
        }
    }
    return size;
}

template <typename StringT>
inline void appendJsonString(StringT& output, std::string_view value) {
    output.push_back('"');
    std::size_t chunkBegin = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!jsonNeedsEscape(c)) {
            continue;
        }
        if (i > chunkBegin) {
            output.append(value.data() + chunkBegin, i - chunkBegin);
        }
        switch (c) {
            case '"':
                output.append("\\\"");
                break;
            case '\\':
                output.append("\\\\");
                break;
            case '\b':
                output.append("\\b");
                break;
            case '\f':
                output.append("\\f");
                break;
            case '\n':
                output.append("\\n");
                break;
            case '\r':
                output.append("\\r");
                break;
            case '\t':
                output.append("\\t");
                break;
            default:
                output.append("\\u00");
                output.push_back(jsonHexDigit(static_cast<std::uint8_t>(c >> 4)));
                output.push_back(jsonHexDigit(static_cast<std::uint8_t>(c & 0x0F)));
                break;
        }
        chunkBegin = i + 1;
    }
    if (chunkBegin < value.size()) {
        output.append(value.data() + chunkBegin, value.size() - chunkBegin);
    }
    output.push_back('"');
}

}  // namespace ruvia::detail
