#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <utility>

#include "ruvia/http/detail/coding/HttpContentCoding.h"
#include "ruvia/http/detail/field/HttpCorsFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"
#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/websocket/HttpWebSocketHandshakeFields.h"

// Submitting a request head as the client: what an outbound :method / :path /
// :authority and its header section must satisfy before the connection will
// encode it, for a regular request, a CONNECT tunnel, and the extended CONNECT
// that carries a WebSocket handshake.

namespace ruvia::detail {
namespace {

struct Http2OutboundRequestHeaderFacts final {
    bool hasContentType{false};
};

[[nodiscard]] bool http2IsValidOutboundMethod(std::string_view method) noexcept {
    return method != "CONNECT" && isValidHttpMethodToken(method);
}

[[nodiscard]] bool http2AreValidOutboundRequestHeaders(
    std::optional<std::string_view> authority,
    std::uint16_t defaultPort,
    std::span<const HttpHeaderView> headers,
    bool allowHost,
    bool allowTrailers,
    HttpRequestContentIndication contentIndication,
    HttpHeaderSectionSize& sectionSize,
    std::size_t generatedFields = 0,
    Http2OutboundRequestHeaderFacts* facts = nullptr) noexcept {
    if (generatedFields > kMaxHttpHeaderFields ||
        headers.size() > kMaxHttpHeaderFields - generatedFields) {
        return false;
    }
    std::uint32_t singletonHeaders = 0;
    bool hostSeen = false;
    bool hasContentType = false;
    HttpRequestExpectations expectations;
    for (const auto& header : headers) {
        if (!http2IsValidRegularHeader(header.name(), header.value()) ||
            !sectionSize.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classifyRequestHeader(header.name());
        if ((kind == RequestHeaderKind::kOrigin &&
             !isValidHttpOriginFieldValue(header.value())) ||
            (kind == RequestHeaderKind::kAccessControlRequestMethod &&
             !isValidHttpCorsRequestMethod(header.value())) ||
            (kind == RequestHeaderKind::kAccessControlRequestHeaders &&
             !isValidHttpCorsRequestHeaderNames(header.value()))) {
            return false;
        }
        // Content-Length belongs exclusively to Http2RequestContent, even when a raw
        // value happens to match. Accepting both would restore two framing truths.
        if (kind == RequestHeaderKind::kContentLength) {
            return false;
        }
        if (!allowTrailers &&
            (header.name() == "te" || header.name() == "trailer")) {
            return false;
        }
        if (kind == RequestHeaderKind::kHost) {
            if (!allowHost || hostSeen || !isValidHostHeader(header.value()) ||
                (authority.has_value() && !authorityMatchesHost(
                    *authority, header.value(), defaultPort))) {
                return false;
            }
            hostSeen = true;
        }
        if (kind == RequestHeaderKind::kContentType) {
            if (hasContentType ||
                !isValidHttpContentTypeFieldValue(header.value())) {
                return false;
            }
            hasContentType = true;
        }
        if (kind == RequestHeaderKind::kContentEncoding &&
            !isValidHttpContentEncodingFieldValue(
                header.value(), HttpFieldListRole::kSender)) {
            return false;
        }
        if (kind == RequestHeaderKind::kExpect) {
            expectations.parseField(header.value());
        }
        if (const auto bit = singletonRequestHeaderBit(kind); bit != 0) {
            if ((singletonHeaders & bit) != 0) {
                return false;
            }
            singletonHeaders |= bit;
        }
    }
    if (!httpClientExpectationIsValid(
            expectations.hasContinue(), contentIndication)) {
        return false;
    }
    if (facts != nullptr) {
        facts->hasContentType = hasContentType;
    }
    return true;
}

[[nodiscard]] bool http2IsValidOutboundRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::optional<std::string_view> authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers,
    bool explicitContent,
    HttpRequestContentIndication contentIndication,
    std::string_view contentLengthValue) noexcept {
    if (!http2IsValidOutboundMethod(method) ||
        !isValidUriScheme(scheme) ||
        (authority.has_value() &&
         !http2IsValidRequestAuthority(scheme, *authority)) ||
        !http2IsValidRegularRequestPath(
            classifyHttpMethod(method), scheme, path) ||
        (path == "*" && authority.has_value()) ||
        (!authority.has_value() &&
         http2RegularRequestRequiresAuthority(scheme, path))) {
        return false;
    }
    const auto defaultPort = httpUriSchemeDefaultPort(scheme);
    HttpHeaderSectionSize sectionSize;
    if (!sectionSize.add(":method", method) ||
        !sectionSize.add(":scheme", scheme) ||
        (authority.has_value() &&
         !sectionSize.add(":authority", *authority)) ||
        !sectionSize.add(":path", path)) {
        return false;
    }
    Http2OutboundRequestHeaderFacts facts;
    if (!http2AreValidOutboundRequestHeaders(
            authority,
            defaultPort,
            headers,
            /*allowHost=*/true,
            /*allowTrailers=*/true,
            contentIndication,
            sectionSize,
            contentLengthValue.empty() ? 0 : 1,
            &facts)) {
        return false;
    }
    if (!contentLengthValue.empty() &&
        !sectionSize.add("content-length", contentLengthValue)) {
        return false;
    }
    if (!explicitContent) {
        return true;
    }
    const auto contentSemantics = httpRequestContentSemantics(method);
    return contentSemantics != HttpRequestContentSemantics::kForbidden &&
        (contentSemantics !=
             HttpRequestContentSemantics::kContentTypeRequired ||
         facts.hasContentType);
}

[[nodiscard]] bool http2IsValidWebSocketConnectHeaders(
    std::span<const HttpHeaderView> headers) noexcept {
    bool sawVersion = false;
    for (const auto& header : headers) {
        if (header.name() == "host" ||
            header.name() == "sec-websocket-key" ||
            header.name() == "sec-websocket-accept") {
            return false;
        }
        if (header.name() == "sec-websocket-version") {
            if (sawVersion || header.value() != "13") {
                return false;
            }
            sawVersion = true;
        }
    }
    return sawVersion && webSocketClientOfferHeadersValid(headers);
}

}  // namespace

Http2RequestHeadSubmitResult Http2Connection::submitRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::optional<std::string_view> authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers,
    Http2RequestContent content) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }
    const bool withoutContent = content.withoutContent() != nullptr;
    const auto* knownLengthContent = content.knownLengthContent();
    const bool streamingContent = content.streamingContent() != nullptr;
    if (!withoutContent && knownLengthContent == nullptr && !streamingContent) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }
    const bool contentWillFollow =
        streamingContent ||
        (knownLengthContent != nullptr && knownLengthContent->length() != 0);
    const auto contentIndication = contentWillFollow
        ? HttpRequestContentIndication::kWillFollow
        : HttpRequestContentIndication::kNoContent;
    Http2EndStream endStream = Http2EndStream::kKeepOpen;
    std::array<char, 20> lengthBuffer{};
    std::size_t lengthBytes = 0;
    if (withoutContent) {
        endStream = Http2EndStream::kEndStream;
    } else if (knownLengthContent != nullptr) {
        endStream = knownLengthContent->length() == 0
            ? Http2EndStream::kEndStream
            : Http2EndStream::kKeepOpen;
        // 20 bytes always hold the canonical decimal form of uint64_t. Do this
        // before mutating stream/HPACK state so every rejection is transactional.
        if (const auto [end, ec] = std::to_chars(
                lengthBuffer.data(),
                lengthBuffer.data() + lengthBuffer.size(),
                knownLengthContent->length());
            ec == std::errc{}) {
            lengthBytes = static_cast<std::size_t>(end - lengthBuffer.data());
        } else {
            return Http2RequestHeadSubmitResult::makeFailure(
                Http2RequestHeadSubmitError::kInvalidMessage);
        }
    }

    const auto contentLengthValue = std::string_view(
        lengthBuffer.data(), lengthBytes);
    // Validate the entire semantic head and decoded-size budget before touching
    // HPACK storage, outbound bytes, stream metadata, or lifecycle state.
    if (!http2IsValidOutboundRegularRequestHead(
            method,
            scheme,
            authority,
            path,
            headers,
            !withoutContent,
            contentIndication,
            contentLengthValue)) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();

    stream->assignRequestMethod(method);
    stream->assignRequestScheme(scheme);
    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", method);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    if (authority.has_value()) {
        HpackEncoder::encodeHeader(block, ":authority", *authority);
    }
    HpackEncoder::encodeHeader(block, ":path", path);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    if (knownLengthContent != nullptr) {
        HpackEncoder::encodeHeaderWithNameIndex(
            block,
            HpackStaticIndex::kContentLength,
            std::string_view(lengthBuffer.data(), lengthBytes));
    }
    appendResponseHeaderFrames(*stream, std::string_view(block), endStream);
    if (withoutContent) {
        stream->beginLocalContentForbidden();
    } else if (knownLengthContent != nullptr) {
        stream->beginLocalContentKnownLength(knownLengthContent->length());
    } else if (streamingContent) {
        stream->beginLocalContentUnbounded();
    }
    if (http2EndsStream(endStream)) {
        (void)stream->commitLocalHeadEndStream();
    } else {
        (void)stream->beginLocalRequestContent();
    }
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

Http2RequestHeadSubmitResult Http2Connection::submitConnectRequestHead(
    std::string_view authority,
    std::span<const HttpHeaderView> headers) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }

    RequestTargetView target;
    HttpHeaderSectionSize sectionSize;
    if (!parseRequestTarget(HttpKnownMethod::kConnect, authority, target) ||
        !sectionSize.add(":method", "CONNECT") ||
        !sectionSize.add(":authority", authority) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            0,
            headers,
            /*allowHost=*/false,
            /*allowTrailers=*/false,
            HttpRequestContentIndication::kNoContent,
            sectionSize)) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();
    (void)stream->beginStandardConnect();

    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":authority", authority);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(block),
        Http2EndStream::kKeepOpen);
    stream->assignRequestMethod("CONNECT");
    stream->beginLocalContentForbidden();
    (void)stream->beginLocalConnectRequest();
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

Http2RequestHeadSubmitResult Http2Connection::submitExtendedConnectRequestHead(
    std::string_view protocol,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }
    if (!peerSettings_.enableConnectProtocol()) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kPeerCapabilityUnavailable);
    }

    const bool websocket = httpAsciiEqualsIgnoreCase(protocol, "websocket");
    const bool websocketScheme =
        httpAsciiEqualsIgnoreCase(scheme, "http") ||
        httpAsciiEqualsIgnoreCase(scheme, "https");
    const auto encodedProtocol = websocket
        ? std::string_view("websocket")
        : protocol;
    HttpHeaderSectionSize sectionSize;
    if (!isValidHttpHeaderName(protocol) ||
        !isValidUriScheme(scheme) ||
        (websocket && !websocketScheme) ||
        !http2IsValidRequestAuthority(scheme, authority) ||
        !http2IsValidExtendedConnectPath(scheme, path) ||
        !sectionSize.add(":method", "CONNECT") ||
        !sectionSize.add(":protocol", encodedProtocol) ||
        !sectionSize.add(":scheme", scheme) ||
        !sectionSize.add(":authority", authority) ||
        !sectionSize.add(":path", path) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            httpUriSchemeDefaultPort(scheme),
            headers,
            /*allowHost=*/!websocket,
            /*allowTrailers=*/false,
            HttpRequestContentIndication::kNoContent,
            sectionSize) ||
        (websocket && !http2IsValidWebSocketConnectHeaders(headers))) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();
    (void)stream->beginExtendedConnect();

    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    // RFC 8441 registers and requires the lowercase `websocket` value. HTTP
    // protocol-name matching is case-insensitive, so accept caller spelling but
    // never put a non-canonical WebSocket token on the wire or in stream state.
    HpackEncoder::encodeHeader(block, ":protocol", encodedProtocol);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    HpackEncoder::encodeHeader(block, ":authority", authority);
    HpackEncoder::encodeHeader(block, ":path", path);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(block),
        Http2EndStream::kKeepOpen);
    stream->assignRequestMethod("CONNECT");
    stream->assignRequestScheme(scheme);
    stream->setProtocol(encodedProtocol);
    stream->beginLocalContentForbidden();
    (void)stream->beginLocalConnectRequest();
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

}  // namespace ruvia::detail
