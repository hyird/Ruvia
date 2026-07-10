#include "ruvia/http/detail/HttpParserInternal.h"

#include "ruvia/http/HttpParser.h"

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
namespace {

using ruvia::detail::authorityMatchesHost;
using ruvia::detail::findHttpHeaderEnd;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::parseHttpHeaderBlock;
using ruvia::detail::parseRequestTarget;
using ruvia::detail::ParsedRequestHeaderBlock;
using ruvia::detail::RequestTargetView;

}  // namespace

void HttpServerParser::parseRequestHead(std::string_view buffer, std::size_t headerSearchOffset, HttpServerParseResult& result) noexcept {
    // Reset only the scalar fields and the reachable request state; the
    // result object is reused across read iterations and requests, so a
    // full value-initialization here would re-zero the 2KB header table.
    result.status = HttpParseStatus::kIncomplete;
    result.error = HttpParseError::kNone;
    result.headerBytes = 0;
    result.contentLength = 0;
    result.consumedBytes = 0;
    result.chunked = false;
    result.responseCoding = HttpContentCoding::kNone;
    result.transferCodings = {};
    result.flags = {};
    HttpRequestAccess::reset(result.request);

    const auto fail = [&result](HttpParseError error) noexcept {
        HttpRequestAccess::reset(result.request);
        result.status = HttpParseStatus::kError;
        result.error = error;
    };

    const auto headerBytes = findHttpHeaderEnd(buffer, headerSearchOffset);
    if (headerBytes == std::string_view::npos) {
        if (buffer.size() >= kMaxHttpHeaderBytes) {
            return fail(HttpParseError::kHeaderTooLarge);
        }
        return;
    }

    if (headerBytes > kMaxHttpHeaderBytes) {
        return fail(HttpParseError::kHeaderTooLarge);
    }

    ParsedRequestHeaderBlock block;
    if (const auto error = parseHttpHeaderBlock(buffer, headerBytes, block); error != HttpParseError::kNone) {
        return fail(error);
    }

    result.headerBytes = headerBytes;
    result.consumedBytes = headerBytes;
    result.flags = block.flags;

    // parseHttpHeaderBlock scans the method through the token table, so it is
    // already a valid non-empty token here.
    const auto method = parseMethod(block.method.bind(buffer));
    if (method == HttpMethod::kUnknown) {
        return fail(HttpParseError::kUnsupportedMethod);
    }
    HttpRequestAccess::setMethod(result.request, method);

    const auto target = block.target.bind(buffer);
    const auto version = block.version.bind(buffer);
    HttpRequestAccess::setTarget(result.request, target);
    HttpRequestAccess::setHttpVersion(result.request, version);

    if (version.size() != 8 ||
        version.substr(0, 5) != "HTTP/" ||
        version[5] < '0' ||
        version[5] > '9' ||
        version[6] != '.') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (version[7] < '0' || version[7] > '9') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
        return fail(HttpParseError::kUnsupportedHttpVersion);
    }

    RequestTargetView targetView;
    if (!parseRequestTarget(method, target, targetView)) {
        return fail(HttpParseError::kInvalidRequestTarget);
    }
    HttpRequestAccess::setPath(result.request, targetView.path);
    HttpRequestAccess::setQueryString(result.request, targetView.query);

    if (version == "HTTP/1.1" && !block.flags.hasHost) {
        return fail(HttpParseError::kMissingHost);
    }
    const auto hostHeaderIndex = block.hostHeaderIndex;
    if (!targetView.authority.empty() && hostHeaderIndex >= 0) {
        const auto hostHeaderValue = block.headers[static_cast<std::size_t>(hostHeaderIndex)].value.bind(buffer);
        if (!authorityMatchesHost(targetView.authority, hostHeaderValue, targetView.defaultPort)) {
            return fail(HttpParseError::kInvalidHost);
        }
    }

    if (block.sawTransferEncoding && block.sawContentLength) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (block.sawTransferEncoding && !block.sawChunked) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (block.sawTransferEncoding && version[7] == '0') {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    for (std::size_t i = 0; i < block.headerCount; ++i) {
        const auto& header = block.headers[i];
        (void)HttpRequestAccess::addHeader(
            result.request,
            HttpHeaderView{header.name.bind(buffer), header.value.bind(buffer)},
            requestHeaderKindKnownSlot(header.kind));
    }

    result.chunked = block.sawChunked;
    result.responseCoding = httpSelectResponseCodingFromQualities(
        block.gzipEncoding, block.brotliEncoding, block.zstdEncoding);
    result.transferCodings = block.transferCodings;
    result.contentLength = block.contentLength;
    result.status = HttpParseStatus::kComplete;
}

void HttpServerParser::parseHeaders(std::string_view buffer, HttpServerParseResult& result, std::size_t headerSearchOffset) const noexcept {
    parseRequestHead(buffer, headerSearchOffset, result);
}

void HttpServerParser::parseBody(std::string_view buffer, HttpServerParseResult& result) const noexcept {
    if (result.status != HttpParseStatus::kComplete) {
        return;
    }

    const auto fail = [&result](HttpParseError error) noexcept {
        HttpRequestAccess::reset(result.request);
        result.status = HttpParseStatus::kError;
        result.error = error;
    };
    const auto needMore = [&result](
        std::size_t headerBytes,
        std::size_t contentLength,
        std::size_t consumedBytes) noexcept {
        HttpRequestAccess::reset(result.request);
        result.status = HttpParseStatus::kIncomplete;
        result.headerBytes = headerBytes;
        result.contentLength = contentLength;
        result.consumedBytes = consumedBytes;
    };

    const auto headerBytes = result.headerBytes;
    const auto contentLength = result.contentLength;
    if (buffer.size() < headerBytes) {
        return needMore(0, 0, 0);
    }
    if (result.chunked) {
        const auto chunked = scanHttpChunkedBody(buffer.substr(headerBytes));
        switch (chunked.status) {
            case HttpChunkScanStatus::kComplete:
                result.consumedBytes = headerBytes + chunked.consumedBytes;
                break;
            case HttpChunkScanStatus::kIncomplete:
                return needMore(headerBytes, 0, 0);
            case HttpChunkScanStatus::kInvalidSize:
                return fail(HttpParseError::kInvalidChunkSize);
            case HttpChunkScanStatus::kSizeOverflow:
                return fail(HttpParseError::kChunkSizeOverflow);
            case HttpChunkScanStatus::kInvalidExtension:
                return fail(HttpParseError::kInvalidChunkExtension);
            case HttpChunkScanStatus::kInvalidCrlf:
                return fail(HttpParseError::kInvalidChunkCrlf);
            case HttpChunkScanStatus::kInvalidTrailer:
                return fail(HttpParseError::kInvalidTrailer);
            case HttpChunkScanStatus::kTooLarge:
                return fail(HttpParseError::kBodyTooLarge);
        }
    } else {
        if (contentLength > kMaxHttpBodyBytes || contentLength > kMaxHttpRequestBytes - headerBytes) {
            return fail(HttpParseError::kBodyTooLarge);
        }
        result.consumedBytes = headerBytes + contentLength;
    }
    if (result.consumedBytes > kMaxHttpRequestBytes) {
        return fail(HttpParseError::kBodyTooLarge);
    }
    if (buffer.size() < result.consumedBytes) {
        return needMore(headerBytes, contentLength, result.consumedBytes);
    }

    HttpRequestAccess::setBody(
        result.request,
        result.chunked ? std::string_view{} : buffer.substr(headerBytes, contentLength));
}

HttpServerParseResult HttpServerParser::parse(std::string_view buffer) const noexcept {
    HttpServerParseResult result;
    parseRequestHead(buffer, 0, result);
    parseBody(buffer, result);
    return result;
}

}  // namespace ruvia::detail

namespace ruvia {

HttpParseResult HttpParser::parse(std::string_view buffer) const noexcept {
    detail::HttpServerParser parser;
    auto parsed = parser.parse(buffer);
    return detail::HttpParseResultAccess::make(
        parsed.status,
        parsed.error,
        parsed.request,
        parsed.consumedBytes);
}

}  // namespace ruvia
