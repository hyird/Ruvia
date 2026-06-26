#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

enum class RequestHeaderKind {
    kOther,
    kAccept,
    kAcceptEncoding,
    kAccessControlRequestHeaders,
    kAccessControlRequestMethod,
    kAuthorization,
    kConnection,
    kContentEncoding,
    kContentLength,
    kContentType,
    kCookie,
    kExpect,
    kHost,
    kIfMatch,
    kIfModifiedSince,
    kIfNoneMatch,
    kIfRange,
    kIfUnmodifiedSince,
    kOrigin,
    kRange,
    kSecWebSocketKey,
    kSecWebSocketProtocol,
    kSecWebSocketVersion,
    kTransferEncoding,
    kUpgrade,
    kUserAgent
};

inline constexpr std::size_t kRequestHeaderKindCount =
    static_cast<std::size_t>(RequestHeaderKind::kUserAgent) + 1;

[[nodiscard]] inline constexpr std::size_t requestHeaderKindKnownSlot(RequestHeaderKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? kRequestHeaderKindCount : index - 1;
}

enum class ChunkSizeLineStatus {
    kOk,
    kInvalidSize,
    kOverflow,
    kInvalidExtension
};

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces multi-comparison chains and lets scan loops validate as they move.
inline constexpr std::array<bool, 256> kHttpTokenCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table[c] = true;
    }
    for (const unsigned char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table[c] = true;
    }
    return table;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> kHttpFieldValueCharTable = [] {
    std::array<bool, 256> table{};
    for (unsigned c = 0; c < 256; ++c) {
        table[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table;
}();

[[nodiscard]] inline bool isHttpTokenChar(unsigned char c) noexcept {
    return kHttpTokenCharTable[c];
}

[[nodiscard]] inline bool isHttpFieldValueChar(unsigned char c) noexcept {
    return kHttpFieldValueCharTable[c];
}

[[nodiscard]] inline bool isHttpHexDigit(unsigned char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

[[nodiscard]] inline std::uint8_t httpHexValue(unsigned char c) noexcept {
    if (c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return static_cast<std::uint8_t>(c - 'a' + 10);
}

[[nodiscard]] RequestHeaderKind classifyRequestHeader(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpChunkExtension(std::string_view value) noexcept;
[[nodiscard]] ChunkSizeLineStatus parseHttpChunkSizeLine(std::string_view value, std::size_t& size) noexcept;

}  // namespace ruvia::detail
