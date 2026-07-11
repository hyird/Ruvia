#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

#include "ruvia/http/Http1RequestParser.h"

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

void Http1ServerRequestParser::parseRequestHead(
    std::string_view buffer,
    std::size_t headerSearchOffset,
    Http1ServerRequestParseState& state) noexcept {
    // Reset only the scalar fields and the reachable request state; the
    // result object is reused across read iterations and requests, so a
    // full value-initialization here would re-zero the 2KB header table.
    state.phase_ = Http1ServerRequestParsePhase::kNeedRequestHead;
    state.error = HttpParseError::kNone;
    state.headerBytes = 0;
    state.messageBytes = 0;
    state.requiredTotalBytes.reset();
    state.bodyPlan = Http1RequestBodyPlan(HttpRequestExpectations{});
    state.connectionPlan = Http1ServerConnectionPlan::close();
    state.responseCoding = HttpContentCoding::kNone;
    HttpRequestAccess::reset(state.request);

    const auto fail = [&state](HttpParseError error) noexcept {
        HttpRequestAccess::reset(state.request);
        state.phase_ = Http1ServerRequestParsePhase::kFailure;
        state.error = error;
        state.messageBytes = 0;
        state.requiredTotalBytes.reset();
        state.connectionPlan = Http1ServerConnectionPlan::close();
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

    state.headerBytes = headerBytes;

    // parseHttpHeaderBlock scans the method through the token table. Preserve the
    // exact wire token: method registration is extensible, while HttpKnownMethod is
    // only the framework's routing/response-semantics classification.
    const auto method = block.method.bind(buffer);
    const auto knownMethod = classifyHttpMethod(method);
    HttpRequestAccess::setMethod(state.request, method);

    const auto target = block.target.bind(buffer);
    const auto version = block.version.bind(buffer);
    HttpRequestAccess::setTarget(state.request, target);

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
    const auto protocolVersion = version[7] == '1'
        ? HttpProtocolVersion::kHttp11
        : HttpProtocolVersion::kHttp10;
    HttpRequestAccess::setProtocolVersion(state.request, protocolVersion);

    RequestTargetView targetView;
    if (!parseRequestTarget(knownMethod, target, targetView)) {
        return fail(HttpParseError::kInvalidRequestTarget);
    }
    HttpRequestAccess::setPath(state.request, targetView.path);
    HttpRequestAccess::setQueryString(state.request, targetView.query);

    if (protocolVersion == HttpProtocolVersion::kHttp11 &&
        block.hostHeaderIndex < 0) {
        return fail(HttpParseError::kMissingHost);
    }
    const auto hostHeaderIndex = block.hostHeaderIndex;
    if (!targetView.authority.empty() && hostHeaderIndex >= 0) {
        const auto hostHeaderValue = block.headers[static_cast<std::size_t>(hostHeaderIndex)].value.bind(buffer);
        if (!authorityMatchesHost(targetView.authority, hostHeaderValue, targetView.defaultPort)) {
            return fail(HttpParseError::kInvalidHost);
        }
    }

    if (block.transferEncoding.present() && block.contentLength.present()) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (block.transferEncoding.present() && !block.transferEncoding.finalChunked()) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (block.transferEncoding.present() &&
        protocolVersion == HttpProtocolVersion::kHttp10) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    for (std::size_t i = 0; i < block.headerCount; ++i) {
        const auto& header = block.headers[i];
        (void)HttpRequestAccess::addHeader(
            state.request,
            HttpHeaderView{header.name.bind(buffer), header.value.bind(buffer)},
            requestHeaderKindKnownSlot(header.kind));
    }

    state.responseCoding = httpSelectResponseCodingFromQualities(
        block.gzipEncoding, block.brotliEncoding, block.zstdEncoding);
    auto expectations = block.expectations;
    if (protocolVersion == HttpProtocolVersion::kHttp10) {
        expectations.ignore100Continue();
    }
    if (block.transferEncoding.finalChunked()) {
        state.bodyPlan = Http1RequestBodyPlan(
            block.transferEncoding.codings(), expectations);
    } else if (block.contentLength.present()) {
        state.bodyPlan = Http1RequestBodyPlan(
            block.contentLength.value(), expectations);
    } else {
        state.bodyPlan = Http1RequestBodyPlan(expectations);
    }
    state.connectionPlan = http1PlanRequestConnection(
        protocolVersion, block.connectionOptions);
    state.phase_ = Http1ServerRequestParsePhase::kRequestHeadReady;
}

void Http1ServerRequestParser::parseHead(
    std::string_view buffer,
    Http1ServerRequestParseState& state,
    std::size_t headerSearchOffset) const noexcept {
    parseRequestHead(buffer, headerSearchOffset, state);
}

void Http1ServerRequestParser::parseMessageBody(
    std::string_view buffer,
    Http1ServerRequestParseState& state) noexcept {
    if (!state.headReady()) {
        return;
    }

    const auto fail = [&state](HttpParseError error) noexcept {
        HttpRequestAccess::reset(state.request);
        state.phase_ = Http1ServerRequestParsePhase::kFailure;
        state.error = error;
        state.messageBytes = 0;
        state.requiredTotalBytes.reset();
        state.bodyPlan = Http1RequestBodyPlan(HttpRequestExpectations{});
        state.connectionPlan = Http1ServerConnectionPlan::close();
    };
    const auto needMore = [&state](
        std::size_t headerBytes,
        Http1RequestBodyPlan bodyPlan,
        std::optional<std::size_t> requiredTotalBytes) noexcept {
        // The request views borrow `buffer`. A caller must reparse after growing
        // or moving that buffer, so an incomplete message intentionally exposes
        // no apparently reusable request head.
        HttpRequestAccess::reset(state.request);
        state.phase_ = Http1ServerRequestParsePhase::kNeedRequestBody;
        state.headerBytes = headerBytes;
        state.bodyPlan = bodyPlan;
        state.messageBytes = 0;
        state.requiredTotalBytes = requiredTotalBytes;
    };

    const auto headerBytes = state.headerBytes;
    const auto bodyPlan = state.bodyPlan;
    const auto* chunkedBody = bodyPlan.chunked();
    const auto* knownLengthBody = bodyPlan.knownLength();
    if (buffer.size() < headerBytes) {
        return needMore(
            0, Http1RequestBodyPlan(HttpRequestExpectations{}), std::nullopt);
    }
    if (chunkedBody != nullptr) {
        const auto chunked = scanHttpChunkedBody(buffer.substr(headerBytes));
        if (const auto* complete = chunked.complete()) {
            state.messageBytes = headerBytes + complete->consumedBytes();
        } else if (chunked.needMore() != nullptr) {
            return needMore(headerBytes, bodyPlan, std::nullopt);
        } else {
            switch (chunked.failure()->error()) {
                case HttpChunkScanError::kInvalidSize:
                    return fail(HttpParseError::kInvalidChunkSize);
                case HttpChunkScanError::kSizeOverflow:
                    return fail(HttpParseError::kChunkSizeOverflow);
                case HttpChunkScanError::kInvalidExtension:
                    return fail(HttpParseError::kInvalidChunkExtension);
                case HttpChunkScanError::kInvalidCrlf:
                    return fail(HttpParseError::kInvalidChunkCrlf);
                case HttpChunkScanError::kInvalidTrailer:
                    return fail(HttpParseError::kInvalidTrailer);
                case HttpChunkScanError::kTooLarge:
                    return fail(HttpParseError::kBodyTooLarge);
            }
        }
    } else if (knownLengthBody != nullptr) {
        const auto contentLength = knownLengthBody->contentLength();
        if (contentLength > kMaxHttpBodyBytes || contentLength > kMaxHttpRequestBytes - headerBytes) {
            return fail(HttpParseError::kBodyTooLarge);
        }
        state.messageBytes = headerBytes + contentLength;
    } else {
        state.messageBytes = headerBytes;
    }
    if (state.messageBytes > kMaxHttpRequestBytes) {
        return fail(HttpParseError::kBodyTooLarge);
    }
    if (buffer.size() < state.messageBytes) {
        return needMore(headerBytes, bodyPlan, state.messageBytes);
    }

    HttpRequestAccess::setBody(
        state.request,
        knownLengthBody != nullptr
            ? buffer.substr(headerBytes, knownLengthBody->contentLength())
            : std::string_view{});
    state.phase_ = Http1ServerRequestParsePhase::kRequestMessageReady;
}

Http1ServerRequestParseState Http1ServerRequestParser::parseMessage(
    std::string_view buffer) const noexcept {
    Http1ServerRequestParseState state;
    parseRequestHead(buffer, 0, state);
    parseMessageBody(buffer, state);
    return state;
}

}  // namespace ruvia::detail

namespace ruvia {

Http1RequestParseResult Http1RequestParser::parse(std::string_view buffer) const noexcept {
    detail::Http1ServerRequestParser parser;
    auto parsed = parser.parseMessage(buffer);
    switch (parsed.phase()) {
        case detail::Http1ServerRequestParsePhase::kNeedRequestHead:
        case detail::Http1ServerRequestParsePhase::kNeedRequestBody:
            return detail::Http1RequestParseResultAccess::needMore(
                parsed.requiredTotalBytes);
        case detail::Http1ServerRequestParsePhase::kFailure:
            return detail::Http1RequestParseResultAccess::failure(parsed.error);
        case detail::Http1ServerRequestParsePhase::kRequestMessageReady:
            break;
        case detail::Http1ServerRequestParsePhase::kRequestHeadReady:
            std::terminate();
    }

    const auto wireBody = buffer.substr(
        parsed.headerBytes,
        parsed.messageBytes - parsed.headerBytes);
    return detail::Http1RequestParseResultAccess::parsed(
        std::move(parsed.request),
        parsed.bodyPlan,
        wireBody,
        parsed.messageBytes);
}

}  // namespace ruvia
