#include "ruvia/http/Http1ServerRequestParser.h"

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/parser/HttpHeaderBlockParser.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"

namespace ruvia {
namespace {

using ruvia::detail::findHttpHeaderEnd;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::ParsedRequestHeaderBlock;
using ruvia::detail::parseHttpHeaderBlock;
using ruvia::detail::parseRequestTarget;
using ruvia::detail::RequestTargetView;
using ruvia::detail::RequestHeaderKind;
using ruvia::detail::singletonRequestHeaderBit;
using ruvia::detail::scanHttpChunkedBody;
using ruvia::detail::HttpChunkScanError;

}  // namespace

void Http1ServerRequestParser::parseRequestHead(std::string_view buffer,
    std::size_t headerSearchOffset, Http1ServerRequestParseState& state,
    std::pmr::memory_resource* resource) {
    // Incomplete input allocates no descriptor storage.
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
                               ? http1PlanHttp11RequestConnection(block.connectionOptions.close())
                               : http1PlanHttp10RequestConnection(block.connectionOptions.close(),
                                     block.connectionOptions.keepAlive());
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

    HttpRequestAccess::setResource(state.request, resource);
    HttpRequestAccess::reserveHeaders(state.request, block.headerCount);
    for (std::size_t i = 0; i < block.headerCount; ++i) {
        const auto& header = block.headers[i];
        auto value = header.value.bind(buffer);
        // RFC 9112 sections 3.2.2 and 3.3 make the request-target authoritative
        // for absolute-form and authority-form. Rebind both headers() and the
        // known-header cache so application code cannot observe a conflicting
        // Host value as a second routing truth.
        if ((targetView.form == detail::HttpRequestTargetForm::kAbsolute ||
                targetView.form == detail::HttpRequestTargetForm::kAuthority) &&
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
    Http1ServerRequestParseState& state, std::size_t headerSearchOffset,
    std::pmr::memory_resource* resource) const {
    parseRequestHead(buffer, headerSearchOffset, state, resource);
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
    std::string_view buffer, std::pmr::memory_resource* resource) const {
    Http1ServerRequestParseState state;
    parseRequestHead(buffer, 0, state, resource);
    parseMessageBody(buffer, state);
    return state;
}

}  // namespace ruvia::detail

namespace ruvia {

std::pair<HttpRequest, std::optional<HttpParseError>> makeParsedHttpRequest(
    std::string_view method, std::string_view target, std::span<const HttpHeaderView> headers,
    std::span<const std::byte> body, std::pmr::memory_resource* resource) {
    auto request = detail::HttpRequestAccess::make();
    detail::HttpRequestAccess::setResource(request, resource);
    detail::HttpRequestAccess::setMethod(request, method);
    detail::HttpRequestAccess::setTarget(request, target);

    std::optional<HttpParseError> error;
    detail::RequestTargetView targetView;
    if (!isValidHttpMethodToken(method)) {
        error = HttpParseError::kInvalidRequestLine;
    } else if (!detail::parseRequestTarget(request.knownMethod(), target, targetView)) {
        error = HttpParseError::kInvalidRequestTarget;
    } else if (headers.size() > kMaxHttpHeaderFields) {
        error = HttpParseError::kTooManyHeaders;
    } else {
        std::size_t headerBytes = 0;
        for (const auto& header : headers) {
            if (!detail::isValidHttpHeaderName(header.name()) ||
                !detail::isValidHttpHeaderValue(header.value())) {
                error = HttpParseError::kInvalidHeader;
                break;
            }
            const auto remaining = kMaxHttpHeaderBytes - headerBytes;
            if (header.name().size() > remaining ||
                header.value().size() > remaining - header.name().size() ||
                remaining - header.name().size() - header.value().size() < 4) {
                error = HttpParseError::kHeaderTooLarge;
                break;
            }
            headerBytes += header.name().size() + header.value().size() + 4;
        }
    }
    if (error.has_value()) {
        return {std::move(request), error};
    }

    detail::HttpRequestAccess::setPath(request, targetView.path);
    detail::HttpRequestAccess::setQueryString(request, targetView.query);
    detail::HttpRequestAccess::setScheme(request, targetView.scheme);
    detail::HttpRequestAccess::setAuthority(request, targetView.authority);
    switch (targetView.form) {
        case detail::HttpRequestTargetForm::kOrigin:
            detail::HttpRequestAccess::setTargetForm(request, ::ruvia::HttpRequestTargetForm::kOrigin);
            break;
        case detail::HttpRequestTargetForm::kAbsolute:
            detail::HttpRequestAccess::setTargetForm(request, ::ruvia::HttpRequestTargetForm::kAbsolute);
            break;
        case detail::HttpRequestTargetForm::kAuthority:
            detail::HttpRequestAccess::setTargetForm(request, ::ruvia::HttpRequestTargetForm::kAuthority);
            break;
        case detail::HttpRequestTargetForm::kAsterisk:
            detail::HttpRequestAccess::setTargetForm(request, ::ruvia::HttpRequestTargetForm::kAsterisk);
            break;
    }
    detail::HttpRequestAccess::reserveHeaders(request, headers.size());
    for (const auto& header : headers) {
        if (!detail::HttpRequestAccess::addHeader(request, header)) {
            error = HttpParseError::kTooManyHeaders;
            break;
        }
    }
    detail::HttpRequestAccess::setBody(request, body);
    return {std::move(request), error};
}

}  // namespace ruvia

namespace ruvia {

bool shouldDropInvalidCleartextHttp1Input(
    std::string_view buffer, Http1RequestParseFailureSource source) noexcept {
    if (source != Http1RequestParseFailureSource::kRequestLine) {
        return false;
    }

    const auto lineEnd = buffer.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        return false;
    }

    auto line = buffer.substr(0, lineEnd);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }

    const auto versionStart = line.find_last_of(" \t");
    if (versionStart == std::string_view::npos || versionStart + 1 >= line.size()) {
        return false;
    }
    return !line.substr(versionStart + 1).starts_with("HTTP/");
}

Http1RequestParseResult Http1RequestParser::parse(std::string_view buffer,
    Http1RequestParseOptions options) const {
    Http1ServerRequestParser parser;
    auto parsed = parser.parseMessage(buffer, options.resource);
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
