#pragma once

#include "ruvia/http/detail/HttpConnectionFields.h"
#include "ruvia/http/detail/HttpContentLength.h"
#include "ruvia/http/detail/HttpExpectations.h"
#include "ruvia/http/detail/HttpTransferEncoding.h"
#include "ruvia/http/detail/HeaderAcceptUtils.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpParseError.h"

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
    std::array<ParsedRequestHeaderSlot, kMaxHttpHeaderFields> headers;
    std::size_t headerCount{0};
    KnownRequestHeaderIndex hostHeaderIndex{-1};
    HttpConnectionOptions connectionOptions;
    HttpUpgradeProtocols upgradeProtocols;
    HttpContentLengthState contentLength;
    std::uint32_t seenHeaderBits{0};
    HttpAcceptedEncodingQuality gzipEncoding;
    HttpAcceptedEncodingQuality brotliEncoding;
    HttpAcceptedEncodingQuality zstdEncoding;
    HttpTransferEncodingState transferEncoding;
    HttpRequestExpectations expectations;
};

[[nodiscard]] std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept;
[[nodiscard]] HttpParseError parseHttpHeaderBlock(
    std::string_view buffer,
    std::size_t headerBytes,
    ParsedRequestHeaderBlock& block) noexcept;

}  // namespace ruvia::detail
