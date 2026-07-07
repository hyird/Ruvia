#pragma once

#include "../HttpRequestFlags.h"
#include "../HttpTransferCoding.h"
#include "../HeaderAcceptUtils.h"
#include "HttpParserSyntax.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpParseTypes.h"

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

struct ParsedRequestHeaderBlock {
    HttpHeaderSlice method;
    HttpHeaderSlice target;
    HttpHeaderSlice version;
    std::array<ParsedRequestHeaderSlot, kMaxRequestHeaders> headers;
    std::size_t headerCount{0};
    KnownRequestHeaderIndex hostHeaderIndex{-1};
    HttpRequestFlags flags;
    std::size_t contentLength{0};
    std::uint32_t seenHeaderBits{0};
    bool sawContentLength{false};
    bool sawChunked{false};
    bool sawTransferEncoding{false};
    HttpAcceptedEncodingQuality gzipEncoding;
    HttpAcceptedEncodingQuality brotliEncoding;
    HttpAcceptedEncodingQuality zstdEncoding;
    HttpTransferCodings transferCodings;
};

[[nodiscard]] std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept;
[[nodiscard]] HttpParseError parseHttpHeaderBlock(
    std::string_view buffer,
    std::size_t headerBytes,
    ParsedRequestHeaderBlock& block) noexcept;

}  // namespace ruvia::detail
