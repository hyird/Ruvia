#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/request_header_kind.h"

namespace ruvia::detail {

[[nodiscard]] inline constexpr std::uint32_t singletonRequestHeaderBit(
    RequestHeaderKind kind) noexcept {
    switch (kind) {
        case RequestHeaderKind::kAccessControlRequestMethod:
        case RequestHeaderKind::kAuthorization:
        case RequestHeaderKind::kContentType:
        case RequestHeaderKind::kIfModifiedSince:
        case RequestHeaderKind::kIfRange:
        case RequestHeaderKind::kIfUnmodifiedSince:
        case RequestHeaderKind::kOrigin:
        case RequestHeaderKind::kRange:
        case RequestHeaderKind::kSecWebSocketKey:
        case RequestHeaderKind::kSecWebSocketVersion:
        case RequestHeaderKind::kUserAgent:
            return 1U << static_cast<std::uint32_t>(kind);
        case RequestHeaderKind::kOther:
        case RequestHeaderKind::kAccept:
        case RequestHeaderKind::kAcceptEncoding:
        case RequestHeaderKind::kAccessControlRequestHeaders:
        case RequestHeaderKind::kConnection:
        case RequestHeaderKind::kContentEncoding:
        case RequestHeaderKind::kContentLength:
        case RequestHeaderKind::kCookie:
        case RequestHeaderKind::kExpect:
        case RequestHeaderKind::kHost:
        case RequestHeaderKind::kIfMatch:
        case RequestHeaderKind::kIfNoneMatch:
        case RequestHeaderKind::kSecWebSocketProtocol:
        case RequestHeaderKind::kTransferEncoding:
        case RequestHeaderKind::kUpgrade:
        case RequestHeaderKind::kForwarded:
        case RequestHeaderKind::kXForwardedFor:
        case RequestHeaderKind::kXForwardedProto:
        case RequestHeaderKind::kSecWebSocketExtensions:
            return 0;
    }
    return 0;
}

enum class ChunkSizeLineStatus : std::uint8_t {
    kOk,
    kInvalidSize,
    kOverflow,
    kInvalidExtension,
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
    for (const unsigned char c :
        {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
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

// Byte repertoire only. Protocol boundaries separately decide whether leading
// or trailing OWS is permitted (HTTP/3 currently accepts it, HTTP/2 does not).
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    for (const unsigned char ch : value) {
        if (!isHttpFieldValueChar(ch)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] RequestHeaderKind classifyRequestHeader(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderName(std::string_view name) noexcept;
[[nodiscard]] bool isValidHttpHeaderValue(std::string_view value) noexcept;
[[nodiscard]] bool isValidHttpChunkExtension(std::string_view value) noexcept;
[[nodiscard]] ChunkSizeLineStatus parseHttpChunkSizeLine(
    std::string_view value, std::size_t& size) noexcept;

}  // namespace ruvia::detail
