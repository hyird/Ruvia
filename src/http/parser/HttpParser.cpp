#include "ruvia/http/HttpParser.h"

#include "HttpParserSyntax.h"
#include "HttpRequestTarget.h"
#include "ruvia/http/HeaderUtils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <system_error>

namespace ruvia {
namespace {

using detail::authorityMatchesHost;
using detail::classifyRequestHeader;
using detail::isHttpFieldValueChar;
using detail::isHttpTokenChar;
using detail::isValidHostHeader;
using detail::isValidHttpChunkExtension;
using detail::parseRequestTarget;
using detail::RequestHeaderKind;
using detail::RequestTargetView;

[[nodiscard]] std::size_t findHeaderEnd(std::string_view buffer, std::size_t searchOffset) noexcept {
    const auto limit = std::min(buffer.size(), kMaxHttpHeaderBytes);
    if (limit < 4) {
        return std::string_view::npos;
    }

    // Hunt for the final '\n' of "\r\n\r\n" with memchr so the scan runs at
    // vectorized libc speed instead of byte-at-a-time comparisons.
    auto cursor = searchOffset >= limit ? limit : std::max<std::size_t>(3, searchOffset + 3);
    while (cursor < limit) {
        const auto* hit = static_cast<const char*>(
            std::memchr(buffer.data() + cursor, '\n', limit - cursor));
        if (hit == nullptr) {
            return std::string_view::npos;
        }
        const auto i = static_cast<std::size_t>(hit - buffer.data());
        if (buffer[i - 1] == '\r' && buffer[i - 2] == '\n' && buffer[i - 3] == '\r') {
            return i + 1;
        }
        cursor = i + 1;
    }

    return std::string_view::npos;
}

// Slice and HeaderSlot carry no default member initializers on purpose: the
// 64-slot header table below stays uninitialized per request (slots are
// always written before they are read up to headerCount), so constructing a
// ParsedHeaderBlock does not re-zero ~1.5KB on every request.
struct Slice {
    std::uint32_t offset;
    std::uint32_t length;

    [[nodiscard]] std::string_view bind(std::string_view buffer) const noexcept {
        return buffer.substr(offset, length);
    }
};

struct HeaderSlot {
    Slice name;
    Slice value;
    RequestHeaderKind kind;
};

struct KnownHeaderIndexes {
    int connection{-1};
    int host{-1};
    int contentLength{-1};
    int transferEncoding{-1};
    int expect{-1};
    int contentType{-1};
    int cookie{-1};
    int origin{-1};
    int accessControlRequestMethod{-1};
    int accessControlRequestHeaders{-1};
    int authorization{-1};
    int acceptEncoding{-1};
    int accept{-1};
    int range{-1};
    int ifMatch{-1};
    int ifNoneMatch{-1};
    int ifModifiedSince{-1};
    int ifUnmodifiedSince{-1};
    int ifRange{-1};
    int upgrade{-1};
    int secWebSocketKey{-1};
    int secWebSocketVersion{-1};
    int secWebSocketProtocol{-1};
    int userAgent{-1};
};

struct ParsedHeaderBlock {
    Slice method;
    Slice target;
    Slice version;
    std::array<HeaderSlot, kMaxRequestHeaders> headers;
    std::size_t headerCount{0};
    KnownHeaderIndexes known;
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

[[nodiscard]] bool parseContentLength(std::string_view value, std::size_t& contentLength) noexcept {
    value = detail::httpTrimOws(value);
    if (value.empty()) {
        return false;
    }

    std::size_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }

    contentLength = parsed;
    return true;
}

enum class TransferEncodingParse {
    kOk,
    kMalformed,
    kUnsupported
};

[[nodiscard]] TransferEncodingParse parseTransferEncoding(std::string_view value, ParsedHeaderBlock& block) noexcept {
    value = detail::httpTrimOws(value);
    for (;;) {
        const auto comma = value.find(',');
        auto token = detail::httpTrimOws(comma == std::string_view::npos ? value : value.substr(0, comma));
        if (const auto semicolon = token.find(';'); semicolon != std::string_view::npos) {
            if (!isValidHttpChunkExtension(token.substr(semicolon))) {
                return TransferEncodingParse::kMalformed;
            }
            token = detail::httpTrimOws(token.substr(0, semicolon));
        }
        if (token.empty()) {
            return TransferEncodingParse::kMalformed;
        }

        if (block.sawChunked) {
            return TransferEncodingParse::kMalformed;
        }

        if (detail::httpAsciiEqualsIgnoreCase(token, "chunked")) {
            block.sawChunked = true;
            block.sawTransferEncoding = true;
            if (comma != std::string_view::npos) {
                return TransferEncodingParse::kMalformed;
            }
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "gzip") ||
                   detail::httpAsciiEqualsIgnoreCase(token, "x-gzip")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.transferGzip = true;
            block.sawTransferEncoding = true;
            block.transferCodings.values[block.transferCodings.count++] = HttpTransferCoding::kGzip;
        } else if (detail::httpAsciiEqualsIgnoreCase(token, "deflate")) {
            if (block.transferCodings.count == kMaxTransferCodings) {
                return TransferEncodingParse::kUnsupported;
            }
            block.transferDeflate = true;
            block.sawTransferEncoding = true;
            block.transferCodings.values[block.transferCodings.count++] = HttpTransferCoding::kDeflate;
        } else {
            return TransferEncodingParse::kUnsupported;
        }
        if (comma == std::string_view::npos) {
            return TransferEncodingParse::kOk;
        }
        value.remove_prefix(comma + 1);
    }
}

[[nodiscard]] Slice makeSlice(std::size_t offset, std::size_t length) noexcept {
    return Slice{
        .offset = static_cast<std::uint32_t>(offset),
        .length = static_cast<std::uint32_t>(length)};
}

[[nodiscard]] std::size_t trimRightOws(std::string_view buffer, std::size_t begin, std::size_t end) noexcept {
    while (end > begin && (buffer[end - 1] == ' ' || buffer[end - 1] == '\t')) {
        --end;
    }
    return end;
}

void recordKnownHeader(ParsedHeaderBlock& block, RequestHeaderKind kind, std::size_t index) noexcept {
    const auto idx = static_cast<int>(index);
    switch (kind) {
        case RequestHeaderKind::kConnection:
            if (block.known.connection < 0) {
                block.known.connection = idx;
            }
            break;
        case RequestHeaderKind::kHost:
            if (block.known.host < 0) {
                block.known.host = idx;
            }
            break;
        case RequestHeaderKind::kContentLength:
            if (block.known.contentLength < 0) {
                block.known.contentLength = idx;
            }
            break;
        case RequestHeaderKind::kTransferEncoding:
            if (block.known.transferEncoding < 0) {
                block.known.transferEncoding = idx;
            }
            break;
        case RequestHeaderKind::kExpect:
            if (block.known.expect < 0) {
                block.known.expect = idx;
            }
            break;
        case RequestHeaderKind::kContentType:
            if (block.known.contentType < 0) {
                block.known.contentType = idx;
            }
            break;
        case RequestHeaderKind::kCookie:
            if (block.known.cookie < 0) {
                block.known.cookie = idx;
            }
            break;
        case RequestHeaderKind::kOrigin:
            if (block.known.origin < 0) {
                block.known.origin = idx;
            }
            break;
        case RequestHeaderKind::kAccessControlRequestMethod:
            if (block.known.accessControlRequestMethod < 0) {
                block.known.accessControlRequestMethod = idx;
            }
            break;
        case RequestHeaderKind::kAccessControlRequestHeaders:
            if (block.known.accessControlRequestHeaders < 0) {
                block.known.accessControlRequestHeaders = idx;
            }
            break;
        case RequestHeaderKind::kAuthorization:
            if (block.known.authorization < 0) {
                block.known.authorization = idx;
            }
            break;
        case RequestHeaderKind::kAcceptEncoding:
            if (block.known.acceptEncoding < 0) {
                block.known.acceptEncoding = idx;
            }
            break;
        case RequestHeaderKind::kAccept:
            if (block.known.accept < 0) {
                block.known.accept = idx;
            }
            break;
        case RequestHeaderKind::kRange:
            if (block.known.range < 0) {
                block.known.range = idx;
            }
            break;
        case RequestHeaderKind::kIfMatch:
            if (block.known.ifMatch < 0) {
                block.known.ifMatch = idx;
            }
            break;
        case RequestHeaderKind::kIfNoneMatch:
            if (block.known.ifNoneMatch < 0) {
                block.known.ifNoneMatch = idx;
            }
            break;
        case RequestHeaderKind::kIfModifiedSince:
            if (block.known.ifModifiedSince < 0) {
                block.known.ifModifiedSince = idx;
            }
            break;
        case RequestHeaderKind::kIfUnmodifiedSince:
            if (block.known.ifUnmodifiedSince < 0) {
                block.known.ifUnmodifiedSince = idx;
            }
            break;
        case RequestHeaderKind::kIfRange:
            if (block.known.ifRange < 0) {
                block.known.ifRange = idx;
            }
            break;
        case RequestHeaderKind::kUpgrade:
            if (block.known.upgrade < 0) {
                block.known.upgrade = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketKey:
            if (block.known.secWebSocketKey < 0) {
                block.known.secWebSocketKey = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketVersion:
            if (block.known.secWebSocketVersion < 0) {
                block.known.secWebSocketVersion = idx;
            }
            break;
        case RequestHeaderKind::kSecWebSocketProtocol:
            if (block.known.secWebSocketProtocol < 0) {
                block.known.secWebSocketProtocol = idx;
            }
            break;
        case RequestHeaderKind::kUserAgent:
            if (block.known.userAgent < 0) {
                block.known.userAgent = idx;
            }
            break;
        case RequestHeaderKind::kOther:
            break;
    }
}

[[nodiscard]] HttpParseError parseHeaderBlock(std::string_view buffer, std::size_t headerBytes, ParsedHeaderBlock& block) noexcept {
    const auto headersEnd = headerBytes - 2;
    std::size_t cursor = 0;

    // Single-pass scans: each loop validates through the character class
    // tables while it advances, so tokens never get re-walked afterwards.
    const auto methodStart = cursor;
    while (cursor < headersEnd && isHttpTokenChar(static_cast<unsigned char>(buffer[cursor]))) {
        ++cursor;
    }
    if (cursor == methodStart || cursor >= headersEnd || buffer[cursor] != ' ') {
        return HttpParseError::kInvalidRequestLine;
    }
    block.method = makeSlice(methodStart, cursor - methodStart);
    ++cursor;

    const auto targetStart = cursor;
    while (cursor < headersEnd && buffer[cursor] != ' ') {
        if (buffer[cursor] == '\r' || buffer[cursor] == '\n') {
            return HttpParseError::kInvalidRequestLine;
        }
        ++cursor;
    }
    if (cursor == targetStart || cursor >= headersEnd) {
        return HttpParseError::kInvalidRequestLine;
    }
    block.target = makeSlice(targetStart, cursor - targetStart);
    ++cursor;

    const auto versionStart = cursor;
    while (cursor < headersEnd && buffer[cursor] != '\r') {
        if (buffer[cursor] == '\n' || buffer[cursor] == ' ') {
            return HttpParseError::kInvalidRequestLine;
        }
        ++cursor;
    }
    if (cursor == versionStart || cursor + 1 >= headersEnd || buffer[cursor + 1] != '\n') {
        return HttpParseError::kInvalidRequestLine;
    }
    block.version = makeSlice(versionStart, cursor - versionStart);
    cursor += 2;

    while (cursor < headersEnd) {
        if (block.headerCount == kMaxRequestHeaders) {
            return HttpParseError::kTooManyHeaders;
        }

        // Field name must be a non-empty token followed by ':'. This also
        // rejects obs-fold continuations (leading SP/HTAB) and stray CRs.
        const auto nameStart = cursor;
        while (cursor < headersEnd && isHttpTokenChar(static_cast<unsigned char>(buffer[cursor]))) {
            ++cursor;
        }
        if (cursor == nameStart || cursor >= headersEnd || buffer[cursor] != ':') {
            return HttpParseError::kInvalidHeader;
        }
        const auto nameEnd = cursor;
        ++cursor;

        while (cursor < headersEnd && (buffer[cursor] == ' ' || buffer[cursor] == '\t')) {
            ++cursor;
        }
        const auto valueStart = cursor;
        while (cursor < headersEnd && isHttpFieldValueChar(static_cast<unsigned char>(buffer[cursor]))) {
            ++cursor;
        }
        if (cursor + 1 >= headersEnd || buffer[cursor] != '\r' || buffer[cursor + 1] != '\n') {
            return HttpParseError::kInvalidHeader;
        }
        const auto valueEnd = trimRightOws(buffer, valueStart, cursor);

        const auto name = buffer.substr(nameStart, nameEnd - nameStart);
        const auto value = buffer.substr(valueStart, valueEnd - valueStart);

        const auto kind = classifyRequestHeader(name);
        switch (kind) {
            case RequestHeaderKind::kHost:
                if (block.flags.hasHost || !isValidHostHeader(value)) {
                    return HttpParseError::kInvalidHost;
                }
                block.flags.hasHost = true;
                break;
            case RequestHeaderKind::kContentLength: {
                std::size_t parsedContentLength = 0;
                if (!parseContentLength(value, parsedContentLength)) {
                    return HttpParseError::kInvalidContentLength;
                }
                if (block.sawContentLength && parsedContentLength != block.contentLength) {
                    return HttpParseError::kConflictingContentLength;
                }
                block.sawContentLength = true;
                block.contentLength = parsedContentLength;
                break;
            }
            case RequestHeaderKind::kTransferEncoding: {
                switch (parseTransferEncoding(value, block)) {
                    case TransferEncodingParse::kOk:
                        break;
                    case TransferEncodingParse::kMalformed:
                        return HttpParseError::kInvalidTransferEncoding;
                    case TransferEncodingParse::kUnsupported:
                        return HttpParseError::kUnsupportedTransferEncoding;
                }
                break;
            }
            case RequestHeaderKind::kConnection:
                detail::httpUpdateConnectionFlags(
                    value,
                    block.flags.connectionClose,
                    block.flags.connectionKeepAlive,
                    block.flags.upgrade);
                break;
            case RequestHeaderKind::kExpect:
                if (!detail::httpUpdateExpectContinueFlag(value, block.flags.expectContinue)) {
                    return HttpParseError::kExpectationFailed;
                }
                break;
            case RequestHeaderKind::kSecWebSocketKey:
                if (block.flags.secWebSocketKeyCount < 2) {
                    ++block.flags.secWebSocketKeyCount;
                }
                break;
            case RequestHeaderKind::kSecWebSocketVersion:
                if (block.flags.secWebSocketVersionCount < 2) {
                    ++block.flags.secWebSocketVersionCount;
                }
                break;
            case RequestHeaderKind::kSecWebSocketProtocol:
                if (block.flags.secWebSocketProtocolCount < 2) {
                    ++block.flags.secWebSocketProtocolCount;
                }
                break;
            case RequestHeaderKind::kAcceptEncoding:
                detail::httpUpdateAcceptedEncodingQuality(
                    value,
                    "gzip",
                    block.acceptGzipQuality,
                    block.acceptGzipWildcardQuality);
                block.flags.acceptsGzip = block.acceptGzipQuality >= 0
                    ? block.acceptGzipQuality > 0
                    : block.acceptGzipWildcardQuality > 0;
                break;
            case RequestHeaderKind::kOther:
            case RequestHeaderKind::kAccept:
            case RequestHeaderKind::kAccessControlRequestHeaders:
            case RequestHeaderKind::kAccessControlRequestMethod:
            case RequestHeaderKind::kAuthorization:
            case RequestHeaderKind::kContentType:
            case RequestHeaderKind::kCookie:
            case RequestHeaderKind::kIfMatch:
            case RequestHeaderKind::kIfModifiedSince:
            case RequestHeaderKind::kIfNoneMatch:
            case RequestHeaderKind::kIfRange:
            case RequestHeaderKind::kIfUnmodifiedSince:
            case RequestHeaderKind::kOrigin:
            case RequestHeaderKind::kRange:
            case RequestHeaderKind::kUpgrade:
            case RequestHeaderKind::kUserAgent:
                break;
        }

        const auto index = block.headerCount++;
        block.headers[index] = HeaderSlot{
            .name = makeSlice(nameStart, nameEnd - nameStart),
            .value = makeSlice(valueStart, valueEnd - valueStart),
            .kind = kind};
        recordKnownHeader(block, kind, index);
        cursor += 2;
    }

    return HttpParseError::kNone;
}

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

    const auto headerBytes = findHeaderEnd(buffer, headerSearchOffset);
    if (headerBytes == std::string_view::npos) {
        if (buffer.size() >= kMaxHttpHeaderBytes) {
            return fail(HttpParseError::kHeaderTooLarge);
        }
        return;
    }

    if (headerBytes > kMaxHttpHeaderBytes) {
        return fail(HttpParseError::kHeaderTooLarge);
    }

    ParsedHeaderBlock block;
    if (const auto error = parseHeaderBlock(buffer, headerBytes, block); error != HttpParseError::kNone) {
        return fail(error);
    }

    result.headerBytes = headerBytes;
    result.consumedBytes = headerBytes;
    result.flags = block.flags;

    // parseHeaderBlock scans the method through the token table, so it is
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
    if (block.known.connection >= 0) {
        result.request.setConnectionHeader(knownValue(block.known.connection));
    }
    if (block.known.host >= 0) {
        result.request.setHostHeader(knownValue(block.known.host));
    }
    if (block.known.contentLength >= 0) {
        result.request.setContentLengthHeader(knownValue(block.known.contentLength));
    }
    if (block.known.transferEncoding >= 0) {
        result.request.setTransferEncodingHeader(knownValue(block.known.transferEncoding));
    }
    if (block.known.expect >= 0) {
        result.request.setExpectHeader(knownValue(block.known.expect));
    }
    if (block.known.contentType >= 0) {
        result.request.setContentTypeHeader(knownValue(block.known.contentType));
    }
    if (block.known.cookie >= 0) {
        result.request.setCookieHeader(knownValue(block.known.cookie));
    }
    if (block.known.origin >= 0) {
        result.request.setOriginHeader(knownValue(block.known.origin));
    }
    if (block.known.accessControlRequestMethod >= 0) {
        result.request.setAccessControlRequestMethodHeader(knownValue(block.known.accessControlRequestMethod));
    }
    if (block.known.accessControlRequestHeaders >= 0) {
        result.request.setAccessControlRequestHeadersHeader(knownValue(block.known.accessControlRequestHeaders));
    }
    if (block.known.authorization >= 0) {
        result.request.setAuthorizationHeader(knownValue(block.known.authorization));
    }
    if (block.known.acceptEncoding >= 0) {
        result.request.setAcceptEncodingHeader(knownValue(block.known.acceptEncoding));
    }
    if (block.known.accept >= 0) {
        result.request.setAcceptHeader(knownValue(block.known.accept));
    }
    if (block.known.range >= 0) {
        result.request.setRangeHeader(knownValue(block.known.range));
    }
    if (block.known.ifMatch >= 0) {
        result.request.setIfMatchHeader(knownValue(block.known.ifMatch));
    }
    if (block.known.ifNoneMatch >= 0) {
        result.request.setIfNoneMatchHeader(knownValue(block.known.ifNoneMatch));
    }
    if (block.known.ifModifiedSince >= 0) {
        result.request.setIfModifiedSinceHeader(knownValue(block.known.ifModifiedSince));
    }
    if (block.known.ifUnmodifiedSince >= 0) {
        result.request.setIfUnmodifiedSinceHeader(knownValue(block.known.ifUnmodifiedSince));
    }
    if (block.known.ifRange >= 0) {
        result.request.setIfRangeHeader(knownValue(block.known.ifRange));
    }
    if (block.known.upgrade >= 0) {
        result.request.setUpgradeHeader(knownValue(block.known.upgrade));
    }
    if (block.known.secWebSocketKey >= 0) {
        result.request.setSecWebSocketKeyHeader(knownValue(block.known.secWebSocketKey));
    }
    if (block.known.secWebSocketVersion >= 0) {
        result.request.setSecWebSocketVersionHeader(knownValue(block.known.secWebSocketVersion));
    }
    if (block.known.secWebSocketProtocol >= 0) {
        result.request.setSecWebSocketProtocolHeader(knownValue(block.known.secWebSocketProtocol));
    }
    if (block.known.userAgent >= 0) {
        result.request.setUserAgentHeader(knownValue(block.known.userAgent));
    }

    if (result.request.httpVersion() == "HTTP/1.1" && !block.flags.hasHost) {
        return fail(HttpParseError::kMissingHost);
    }
    if (!targetView.authority.empty() &&
        block.known.host >= 0 &&
        !authorityMatchesHost(targetView.authority, knownValue(block.known.host), targetView.defaultPort)) {
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
