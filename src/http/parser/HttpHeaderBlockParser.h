#pragma once

#include "HttpParserSyntax.h"
#include "ruvia/http/HttpParser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

// Slice and ParsedRequestHeaderSlot carry no default member initializers on
// purpose: the fixed header table stays uninitialized per request (slots are
// always written before they are read up to headerCount), so constructing a
// ParsedRequestHeaderBlock does not re-zero ~1.5KB on every request.
struct HttpHeaderSlice {
    std::uint32_t offset;
    std::uint32_t length;

    [[nodiscard]] std::string_view bind(std::string_view buffer) const noexcept {
        return buffer.substr(offset, length);
    }
};

struct ParsedRequestHeaderSlot {
    HttpHeaderSlice name;
    HttpHeaderSlice value;
    RequestHeaderKind kind;
};

using KnownRequestHeaderIndex = std::int16_t;
inline constexpr KnownRequestHeaderIndex kMissingRequestHeaderIndex = -1;

struct KnownRequestHeaderIndexes {
    std::array<KnownRequestHeaderIndex, kRequestHeaderKindCount> values = [] {
        std::array<KnownRequestHeaderIndex, kRequestHeaderKindCount> indexes{};
        indexes.fill(kMissingRequestHeaderIndex);
        return indexes;
    }();

    [[nodiscard]] KnownRequestHeaderIndex get(RequestHeaderKind kind) const noexcept {
        return values[static_cast<std::size_t>(kind)];
    }

    void record(RequestHeaderKind kind, std::size_t index) noexcept {
        auto& knownIndex = values[static_cast<std::size_t>(kind)];
        if (knownIndex == kMissingRequestHeaderIndex) {
            knownIndex = static_cast<KnownRequestHeaderIndex>(index);
        }
    }
};

struct ParsedRequestHeaderBlock {
    HttpHeaderSlice method;
    HttpHeaderSlice target;
    HttpHeaderSlice version;
    std::array<ParsedRequestHeaderSlot, kMaxRequestHeaders> headers;
    std::size_t headerCount{0};
    KnownRequestHeaderIndexes known;
    HttpRequestFlags flags;
    std::size_t contentLength{0};
    bool sawContentLength{false};
    bool sawChunked{false};
    bool sawTransferEncoding{false};
    bool transferGzip{false};
    bool transferDeflate{false};
    int acceptGzipQuality{-1};
    int acceptGzipWildcardQuality{-1};
    HttpTransferCodings transferCodings;
};

[[nodiscard]] std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept;
[[nodiscard]] HttpParseError parseHttpHeaderBlock(
    std::string_view buffer,
    std::size_t headerBytes,
    ParsedRequestHeaderBlock& block) noexcept;

}  // namespace ruvia::detail
