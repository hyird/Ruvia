#include "ruvia/http/HttpParser.h"

#include "HttpHeaderBlockParser.h"
#include "HttpRequestTarget.h"

namespace ruvia {
namespace {

using detail::authorityMatchesHost;
using detail::findHttpHeaderEnd;
using detail::parseHttpHeaderBlock;
using detail::parseRequestTarget;
using detail::ParsedRequestHeaderBlock;
using detail::RequestHeaderKind;
using detail::RequestTargetView;

}  // namespace

void HttpParser::parseRequestHead(std::string_view buffer, std::size_t headerSearchOffset, HttpParseResult& result) noexcept {
    // Reset only the scalar fields and the reachable request state; the
    // result object is reused across read iterations and requests, so a
    // full value-initialization here would re-zero the 2KB header table.
    result.status = HttpParseStatus::kIncomplete;
    result.error = HttpParseError::kNone;
    result.headerBytes = 0;
    result.contentLength = 0;
    result.decodedBodyBytes = 0;
    result.consumedBytes = 0;
    result.chunked = false;
    result.transferGzip = false;
    result.transferDeflate = false;
    result.transferCodings = {};
    result.flags = {};
    result.bodyPlan = {};
    result.request.reset();

    const auto fail = [&result](HttpParseError error) noexcept {
        result.request.reset();
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
    result.request.setMethod(parseMethod(block.method.bind(buffer)));
    if (result.request.method() == HttpMethod::kUnknown) {
        return fail(HttpParseError::kUnsupportedMethod);
    }

    const auto target = block.target.bind(buffer);
    result.request.setTarget(target);
    result.request.setHttpVersion(block.version.bind(buffer));

    if (result.request.httpVersion().size() != 8 ||
        result.request.httpVersion().substr(0, 5) != "HTTP/" ||
        result.request.httpVersion()[5] < '0' ||
        result.request.httpVersion()[5] > '9' ||
        result.request.httpVersion()[6] != '.') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (result.request.httpVersion()[7] < '0' || result.request.httpVersion()[7] > '9') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (result.request.httpVersion()[5] != '1' ||
        (result.request.httpVersion()[7] != '0' && result.request.httpVersion()[7] != '1')) {
        return fail(HttpParseError::kUnsupportedHttpVersion);
    }

    RequestTargetView targetView;
    if (!parseRequestTarget(result.request.method(), target, targetView)) {
        return fail(HttpParseError::kInvalidRequestTarget);
    }
    result.request.setPath(targetView.path);
    result.request.setQueryString(targetView.query);

    const auto knownValue = [&block, buffer](int index) noexcept -> std::string_view {
        return index < 0 ? std::string_view{} : block.headers[static_cast<std::size_t>(index)].value.bind(buffer);
    };
    for (std::size_t i = 0; i < block.headerCount; ++i) {
        (void)result.request.addHeader(HttpHeaderView{block.headers[i].name.bind(buffer), block.headers[i].value.bind(buffer)});
    }
    const auto knownIndex = [&block](RequestHeaderKind kind) noexcept {
        return block.known.get(kind);
    };
    const auto applyKnown = [&result, &knownValue](int index, auto setter) noexcept {
        if (index >= 0) {
            (result.request.*setter)(knownValue(index));
        }
    };
    applyKnown(knownIndex(RequestHeaderKind::kConnection), &HttpRequest::setConnectionHeader);
    applyKnown(knownIndex(RequestHeaderKind::kHost), &HttpRequest::setHostHeader);
    applyKnown(knownIndex(RequestHeaderKind::kContentLength), &HttpRequest::setContentLengthHeader);
    applyKnown(knownIndex(RequestHeaderKind::kTransferEncoding), &HttpRequest::setTransferEncodingHeader);
    applyKnown(knownIndex(RequestHeaderKind::kExpect), &HttpRequest::setExpectHeader);
    applyKnown(knownIndex(RequestHeaderKind::kContentType), &HttpRequest::setContentTypeHeader);
    applyKnown(knownIndex(RequestHeaderKind::kCookie), &HttpRequest::setCookieHeader);
    applyKnown(knownIndex(RequestHeaderKind::kOrigin), &HttpRequest::setOriginHeader);
    applyKnown(knownIndex(RequestHeaderKind::kAccessControlRequestMethod), &HttpRequest::setAccessControlRequestMethodHeader);
    applyKnown(knownIndex(RequestHeaderKind::kAccessControlRequestHeaders), &HttpRequest::setAccessControlRequestHeadersHeader);
    applyKnown(knownIndex(RequestHeaderKind::kAuthorization), &HttpRequest::setAuthorizationHeader);
    applyKnown(knownIndex(RequestHeaderKind::kAcceptEncoding), &HttpRequest::setAcceptEncodingHeader);
    applyKnown(knownIndex(RequestHeaderKind::kAccept), &HttpRequest::setAcceptHeader);
    applyKnown(knownIndex(RequestHeaderKind::kRange), &HttpRequest::setRangeHeader);
    applyKnown(knownIndex(RequestHeaderKind::kIfMatch), &HttpRequest::setIfMatchHeader);
    applyKnown(knownIndex(RequestHeaderKind::kIfNoneMatch), &HttpRequest::setIfNoneMatchHeader);
    applyKnown(knownIndex(RequestHeaderKind::kIfModifiedSince), &HttpRequest::setIfModifiedSinceHeader);
    applyKnown(knownIndex(RequestHeaderKind::kIfUnmodifiedSince), &HttpRequest::setIfUnmodifiedSinceHeader);
    applyKnown(knownIndex(RequestHeaderKind::kIfRange), &HttpRequest::setIfRangeHeader);
    applyKnown(knownIndex(RequestHeaderKind::kUpgrade), &HttpRequest::setUpgradeHeader);
    applyKnown(knownIndex(RequestHeaderKind::kSecWebSocketKey), &HttpRequest::setSecWebSocketKeyHeader);
    applyKnown(knownIndex(RequestHeaderKind::kSecWebSocketVersion), &HttpRequest::setSecWebSocketVersionHeader);
    applyKnown(knownIndex(RequestHeaderKind::kSecWebSocketProtocol), &HttpRequest::setSecWebSocketProtocolHeader);
    applyKnown(knownIndex(RequestHeaderKind::kUserAgent), &HttpRequest::setUserAgentHeader);

    if (result.request.httpVersion() == "HTTP/1.1" && !block.flags.hasHost) {
        return fail(HttpParseError::kMissingHost);
    }
    const auto hostHeaderIndex = knownIndex(RequestHeaderKind::kHost);
    if (!targetView.authority.empty() &&
        hostHeaderIndex >= 0 &&
        !authorityMatchesHost(targetView.authority, knownValue(hostHeaderIndex), targetView.defaultPort)) {
        return fail(HttpParseError::kInvalidHost);
    }

    if (block.sawTransferEncoding && block.sawContentLength) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (block.sawTransferEncoding && !block.sawChunked) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (block.sawTransferEncoding && result.request.httpVersion()[7] == '0') {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    result.chunked = block.sawChunked;
    result.transferGzip = block.transferGzip;
    result.transferDeflate = block.transferDeflate;
    result.transferCodings = block.transferCodings;
    result.contentLength = block.contentLength;
    result.flags.transferGzip = block.transferGzip;
    result.flags.transferDeflate = block.transferDeflate;
    result.bodyPlan = HttpBodyPlan{
        .kind = block.sawChunked ? HttpBodyKind::kChunked : (block.contentLength == 0 ? HttpBodyKind::kNone : HttpBodyKind::kContentLength),
        .contentLength = block.contentLength,
        .expectContinue = block.flags.expectContinue};
    result.status = HttpParseStatus::kComplete;
}

void HttpParser::parseHeaders(std::string_view buffer, HttpParseResult& result, std::size_t headerSearchOffset) const noexcept {
    parseRequestHead(buffer, headerSearchOffset, result);
}

void HttpParser::parseBody(std::string_view buffer, HttpParseResult& result) const noexcept {
    if (result.status != HttpParseStatus::kComplete) {
        return;
    }

    const auto fail = [&result](HttpParseError error) noexcept {
        result.request.reset();
        result.status = HttpParseStatus::kError;
        result.error = error;
    };
    const auto needMore = [&result](
        std::size_t headerBytes,
        std::size_t contentLength,
        std::size_t consumedBytes) noexcept {
        result.request.reset();
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
                result.decodedBodyBytes = chunked.decodedBytes;
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
        result.decodedBodyBytes = contentLength;
        result.consumedBytes = headerBytes + contentLength;
    }
    if (result.consumedBytes > kMaxHttpRequestBytes) {
        return fail(HttpParseError::kBodyTooLarge);
    }
    if (buffer.size() < result.consumedBytes) {
        return needMore(headerBytes, contentLength, result.consumedBytes);
    }

    result.request.setBody(
        result.chunked ? std::string_view{} : buffer.substr(headerBytes, contentLength));
}

HttpParseResult HttpParser::parse(std::string_view buffer) const noexcept {
    HttpParseResult result;
    parseRequestHead(buffer, 0, result);
    parseBody(buffer, result);
    return result;
}

}  // namespace ruvia
