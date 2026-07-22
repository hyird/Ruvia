#pragma once

#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/coding/HttpContentLength.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/coding/HttpTransferEncoding.h"
#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpParseError.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

    template <HttpTemporaryOwningCharString Buffer>
    std::string_view bind(Buffer&&) const = delete;
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
    HttpResponseCodingQualities responseCodingQualities;
    HttpTransferEncodingState transferEncoding;
    HttpRequestExpectations expectations;
};

[[nodiscard]] std::size_t findHttpHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept;
[[nodiscard]] std::optional<HttpParseError> parseHttpHeaderBlock(
    std::string_view buffer,
    std::size_t headerBytes,
    ParsedRequestHeaderBlock& block) noexcept;

}  // namespace ruvia::detail
