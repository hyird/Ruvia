#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

#include "ruvia/http/Http1RequestParser.h"

#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {
namespace {

using ruvia::detail::findHttpHeaderEnd;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::HttpRequestTargetForm;
using ruvia::detail::ParsedRequestHeaderBlock;
using ruvia::detail::parseHttpHeaderBlock;
using ruvia::detail::parseRequestTarget;
using ruvia::detail::RequestTargetView;

}  // namespace

void Http1ServerRequestParser::parseRequestHead(std::string_view buffer,
    std::size_t headerSearchOffset, Http1ServerRequestParseState& state) noexcept {
    // Reset only the small progress value and reachable request state; the
    // result object is reused across read iterations and requests, so a full
    // value-initialization here would re-zero the 2KB header table.
    state.progress_ = Http1ServerNeedRequestHead{};
    state.bodyPlan = Http1RequestBodyPlan(HttpRequestExpectations{});
    state.connectionPlan = Http1ServerConnectionPlan::http11Close();
    state.responseCodingQualities = {};
    HttpRequestAccess::reset(state.request);

    const auto fail = [&state](HttpParseError error) noexcept {
        // The request version may already have been accepted when a later
        // target/framing semantic check fails. Preserve that protocol contract
        // so an HTTP/1.0 error response is not silently upgraded to HTTP/1.1.
        const auto connectionPlan = state.connectionPlan;
        HttpRequestAccess::reset(state.request);
        state.progress_ = Http1ServerRequestParseFailure(error);
        state.connectionPlan = connectionPlan;
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
    if (const auto error = parseHttpHeaderBlock(buffer, headerBytes, block)) {
        return fail(*error);
    }

    // parseHttpHeaderBlock scans the method through the token table. Preserve the
    // exact wire token: method registration is extensible, while HttpKnownMethod is
    // only the framework's routing/response-semantics classification.
    const auto method = block.method.bind(buffer);
    const auto knownMethod = classifyHttpMethod(method);
    HttpRequestAccess::setMethod(state.request, method);

    const auto target = block.target.bind(buffer);
    const auto version = block.version.bind(buffer);
    HttpRequestAccess::setTarget(state.request, target);

    if (version.size() != 8 || !version.starts_with("HTTP/") || version[5] < '0' ||
        version[5] > '9' || version[6] != '.') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (version[7] < '0' || version[7] > '9') {
        return fail(HttpParseError::kInvalidRequestLine);
    }
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) {
        return fail(HttpParseError::kUnsupportedHttpVersion);
    }
    const auto protocolVersion =
        version[7] == '1' ? HttpProtocolVersion::kHttp11 : HttpProtocolVersion::kHttp10;
    HttpRequestAccess::setProtocolVersion(state.request, protocolVersion);
    // Publish the version-specific request contract before any validation that
    // can fail after the version line itself has been accepted. The final
    // disposition is already derived from the parsed Connection fields and is
    // tightened to close by body/response policy later.
    state.connectionPlan = protocolVersion == HttpProtocolVersion::kHttp11
                               ? http1PlanHttp11RequestConnection(block.connectionOptions)
                               : http1PlanHttp10RequestConnection(block.connectionOptions);
    if (block.upgradeProtocols.hasField() && !block.connectionOptions.upgrade()) {
        return fail(HttpParseError::kInvalidConnection);
    }
    if (block.teHeaderPresent && !block.connectionOptions.te()) {
        return fail(HttpParseError::kInvalidConnection);
    }

    RequestTargetView targetView;
    if (!parseRequestTarget(knownMethod, target, targetView)) {
        return fail(HttpParseError::kInvalidRequestTarget);
    }
    HttpRequestAccess::setPath(state.request, targetView.path);
    HttpRequestAccess::setQueryString(state.request, targetView.query);
    HttpRequestAccess::setScheme(state.request, targetView.scheme);
    HttpRequestAccess::setAuthority(state.request, targetView.authority);
    switch (targetView.form) {
        case detail::HttpRequestTargetForm::kOrigin:
            HttpRequestAccess::setTargetForm(
                state.request, ::ruvia::HttpRequestTargetForm::kOrigin);
            break;
        case detail::HttpRequestTargetForm::kAbsolute:
            HttpRequestAccess::setTargetForm(
                state.request, ::ruvia::HttpRequestTargetForm::kAbsolute);
            break;
        case detail::HttpRequestTargetForm::kAuthority:
            HttpRequestAccess::setTargetForm(
                state.request, ::ruvia::HttpRequestTargetForm::kAuthority);
            break;
        case detail::HttpRequestTargetForm::kAsterisk:
            HttpRequestAccess::setTargetForm(
                state.request, ::ruvia::HttpRequestTargetForm::kAsterisk);
            break;
    }

    if (protocolVersion == HttpProtocolVersion::kHttp11 && block.hostHeaderIndex < 0) {
        return fail(HttpParseError::kMissingHost);
    }
    const auto hostHeaderIndex = block.hostHeaderIndex;

    const auto contentLength = block.contentLength.value();
    const auto transferEncoding = block.transferEncoding.value();
    if (transferEncoding.has_value() && contentLength.has_value()) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (httpRequestContentSemantics(method) == HttpRequestContentSemantics::kForbidden) {
        // CONNECT has no request content, and TRACE explicitly forbids it
        // (RFC 9110 sections 9.3.6 and 9.3.8). Content-Length is an explicit
        // content signal even at zero; accepting either framing field would
        // give the runtime a body contract that the method does not have.
        if (transferEncoding.has_value()) {
            return fail(HttpParseError::kInvalidTransferEncoding);
        }
        if (contentLength.has_value()) {
            return fail(HttpParseError::kInvalidContentLength);
        }
    }

    const auto* finalChunked =
        transferEncoding.has_value() ? transferEncoding->finalChunked() : nullptr;
    if (transferEncoding.has_value() && finalChunked == nullptr) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }
    if (block.nonEmptyTrailerHeaderPresent && finalChunked == nullptr) {
        return fail(HttpParseError::kInvalidHeader);
    }

    // RFC 9112 section 6.1: Transfer-Encoding in an HTTP/1.0 request must be treated
    // as faulty framing; the error path closes the connection after replying.
    if (transferEncoding.has_value() && protocolVersion == HttpProtocolVersion::kHttp10) {
        return fail(HttpParseError::kInvalidTransferEncoding);
    }

    if (httpRequestContentSemantics(method) == HttpRequestContentSemantics::kContentTypeRequired &&
        (contentLength.has_value() || transferEncoding.has_value()) &&
        (block.seenHeaderBits & singletonRequestHeaderBit(RequestHeaderKind::kContentType)) == 0) {
        // RFC 9110 section 9.3.7 requires a valid Content-Type when OPTIONS
        // explicitly carries content. A zero Content-Length still declares an
        // empty representation and therefore retains this metadata contract.
        return fail(HttpParseError::kInvalidHeader);
    }

    for (std::size_t i = 0; i < block.headerCount; ++i) {
        const auto& header = block.headers[i];
        auto value = header.value.bind(buffer);
        // RFC 9112 sections 3.2.2 and 3.3 make the request-target authoritative
        // for absolute-form and authority-form. Rebind both headers() and the
        // known-header cache so application code cannot observe a conflicting
        // Host value as a second routing truth.
        if ((targetView.form == HttpRequestTargetForm::kAbsolute ||
                targetView.form == HttpRequestTargetForm::kAuthority) &&
            hostHeaderIndex >= 0 && i == static_cast<std::size_t>(hostHeaderIndex)) {
            value = targetView.authority;
        }
        (void)HttpRequestAccess::addHeader(state.request,
            HttpHeaderView{header.name.bind(buffer), value},
            requestHeaderKindKnownSlot(header.kind));
    }

    state.responseCodingQualities = block.responseCodingQualities;
    auto expectations = block.expectations;
    if (protocolVersion == HttpProtocolVersion::kHttp10) {
        expectations.ignoreContinue();
    }
    if (finalChunked != nullptr) {
        state.bodyPlan = Http1RequestBodyPlan(finalChunked->transferCodings(), expectations);
    } else if (contentLength.has_value()) {
        state.bodyPlan = Http1RequestBodyPlan(*contentLength, expectations);
    } else {
        state.bodyPlan = Http1RequestBodyPlan(expectations);
    }
    state.progress_ = Http1ServerRequestHeadReady(headerBytes);
}

void Http1ServerRequestParser::parseHead(std::string_view buffer,
    Http1ServerRequestParseState& state, std::size_t headerSearchOffset) const noexcept {
    parseRequestHead(buffer, headerSearchOffset, state);
}

void Http1ServerRequestParser::parseMessageBody(
    std::string_view buffer, Http1ServerRequestParseState& state) noexcept {
    const auto* requestHead = state.headReady();
    if (requestHead == nullptr) {
        return;
    }

    const auto headerBytes = requestHead->headerBytes();

    const auto fail = [&state](HttpParseError error) noexcept {
        const auto connectionPlan = state.connectionPlan.requireClose();
        HttpRequestAccess::reset(state.request);
        state.progress_ = Http1ServerRequestParseFailure(error);
        state.bodyPlan = Http1RequestBodyPlan(HttpRequestExpectations{});
        state.connectionPlan = connectionPlan;
    };
    // headerBytes is captured rather than passed: every call site forwards the
    // same head length, and a parameter of that name would shadow it.
    const auto needMore = [&state, headerBytes](Http1RequestBodyPlan bodyPlan) noexcept {
        // The request views borrow `buffer`. A caller must reparse after growing
        // or moving that buffer, so an incomplete message intentionally exposes
        // no apparently reusable request head.
        HttpRequestAccess::reset(state.request);
        state.progress_ = Http1ServerNeedRequestBody(headerBytes);
        state.bodyPlan = bodyPlan;
    };
    const auto needMoreUntil = [&state, headerBytes](Http1RequestBodyPlan bodyPlan,
                                   std::size_t requiredTotalBytes) noexcept {
        // The request views borrow `buffer`. A caller must reparse after growing
        // or moving that buffer, so an incomplete message intentionally exposes
        // no apparently reusable request head.
        HttpRequestAccess::reset(state.request);
        state.progress_ = Http1ServerNeedRequestBody(headerBytes, requiredTotalBytes);
        state.bodyPlan = bodyPlan;
    };

    const auto bodyPlan = state.bodyPlan;
    const auto* chunkedBody = bodyPlan.chunked();
    const auto* knownLengthBody = bodyPlan.knownLength();
    std::size_t messageBytes = 0;
    if (chunkedBody != nullptr) {
        const auto chunked = scanHttpChunkedBody(buffer.substr(headerBytes));
        if (const auto* complete = chunked.complete()) {
            messageBytes = headerBytes + complete->consumedBytes();
        } else if (chunked.needMore() != nullptr) {
            return needMore(bodyPlan);
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
        if (contentLength > kDefaultMaxBufferedBodyBytes ||
            contentLength > kMaxHttpRequestBytes - headerBytes) {
            return fail(HttpParseError::kBodyTooLarge);
        }
        messageBytes = headerBytes + contentLength;
    } else {
        messageBytes = headerBytes;
    }
    if (messageBytes > kMaxHttpRequestBytes) {
        return fail(HttpParseError::kBodyTooLarge);
    }
    if (buffer.size() < messageBytes) {
        return needMoreUntil(bodyPlan, messageBytes);
    }

    HttpRequestAccess::setBody(state.request,
        knownLengthBody != nullptr ? buffer.substr(headerBytes, knownLengthBody->contentLength())
                                   : std::string_view{});
    state.progress_ = Http1ServerRequestMessageReady(headerBytes, messageBytes);
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
    if (parsed.needRequestHead() != nullptr) {
        return detail::Http1RequestParseResultAccess::needMore();
    }
    if (const auto* needBody = parsed.needRequestBody()) {
        if (const auto requiredTotalBytes = needBody->requiredTotalBytes()) {
            return detail::Http1RequestParseResultAccess::needMore(*requiredTotalBytes);
        }
        return detail::Http1RequestParseResultAccess::needMore();
    }
    if (const auto* failure = parsed.failure()) {
        return detail::Http1RequestParseResultAccess::failure(*failure);
    }
    const auto* message = parsed.messageReady();
    if (message == nullptr) {
        std::terminate();
    }

    const auto wireBody =
        buffer.substr(message->headerBytes(), message->messageBytes() - message->headerBytes());
    return detail::Http1RequestParseResultAccess::parsed(
        std::move(parsed.request), parsed.bodyPlan, wireBody, message->messageBytes());
}

}  // namespace ruvia
