#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/http2/Http2FlowControl.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2IsValidOutboundMethod(std::string_view method) noexcept {
    return method != "CONNECT" && isValidHttpMethodToken(method);
}

[[nodiscard]] bool http2AreValidOutboundRequestHeaders(
    std::string_view authority,
    std::uint16_t defaultPort,
    std::span<const HttpHeaderView> headers,
    bool allowHost,
    bool allowTrailers) noexcept {
    std::uint32_t singletonHeaders = 0;
    bool hostSeen = false;
    for (const auto& header : headers) {
        if (!http2IsValidRegularHeader(header.name(), header.value())) {
            return false;
        }
        const auto kind = classifyRequestHeader(header.name());
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
                !authorityMatchesHost(authority, header.value(), defaultPort)) {
                return false;
            }
            hostSeen = true;
        }
        if (const auto bit = singletonRequestHeaderBit(kind); bit != 0) {
            if ((singletonHeaders & bit) != 0) {
                return false;
            }
            singletonHeaders |= bit;
        }
    }
    return true;
}

[[nodiscard]] bool http2IsValidOutboundRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers) noexcept {
    if (!http2IsValidOutboundMethod(method) ||
        !isValidUriScheme(scheme) ||
        !isValidHostHeader(authority) ||
        !isValidOriginOrAsteriskFormTarget(
            classifyHttpMethod(method), path)) {
        return false;
    }
    const auto defaultPort = httpUriSchemeDefaultPort(scheme);
    return http2AreValidOutboundRequestHeaders(
        authority, defaultPort, headers, /*allowHost=*/true, /*allowTrailers=*/true);
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
    return sawVersion;
}

[[nodiscard]] bool http2IsValidConnectResponseHead(
    const HttpResponse& response) noexcept {
    const auto& body = responseBody(response);
    if (response.status() < 200 || response.status() >= 300 ||
        body.size() != 0 || body.file().has_value()) {
        return false;
    }
    for (const auto& header : response.headers()) {
        const auto known = responseHeaderKnownBit(header);
        if (known == kResponseHeaderContentLength ||
            known == kResponseHeaderTransferEncoding ||
            httpAsciiEqualsIgnoreCase(header.name(), "content-length") ||
            httpAsciiEqualsIgnoreCase(header.name(), "transfer-encoding")) {
            return false;
        }
    }
    return true;
}
}  // namespace

Http2RequestHeadSubmitResult Http2Connection::submitRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers,
    Http2RequestContent content) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }
    // Validate the entire semantic head before touching HPACK storage, outbound
    // bytes, the stream method, local-content accounting, or lifecycle state.
    if (!http2IsValidOutboundRegularRequestHead(method, scheme, authority, path, headers)) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    Http2EndStream endStream = Http2EndStream::kKeepOpen;
    std::array<char, 20> lengthBuffer{};
    std::size_t lengthBytes = 0;
    const bool withoutContent = content.withoutContent() != nullptr;
    const auto* knownLengthContent = content.knownLengthContent();
    const bool streamingContent = content.streamingContent() != nullptr;
    if (!withoutContent && knownLengthContent == nullptr && !streamingContent) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }
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
    HpackEncoder::encodeHeader(block, ":authority", authority);
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
    appendResponseHeaderFrames(
        *stream, std::string_view(block.data(), block.size()), endStream);
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
    if (!parseRequestTarget(HttpKnownMethod::kConnect, authority, target) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            0,
            headers,
            /*allowHost=*/false,
            /*allowTrailers=*/false)) {
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
        std::string_view(block.data(), block.size()),
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
    if (!isValidHttpHeaderName(protocol) ||
        !isValidUriScheme(scheme) ||
        (websocket && !websocketScheme) ||
        !isValidHostHeader(authority) ||
        !isValidOriginFormTarget(path) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            httpUriSchemeDefaultPort(scheme),
            headers,
            /*allowHost=*/!websocket,
            /*allowTrailers=*/false) ||
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
        std::string_view(block.data(), block.size()),
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

void Http2Connection::appendResponseHeaderFrames(
    Http2StreamState& stream,
    std::string_view headerBlock,
    Http2EndStream endStream) {
    // A HEADERS + CONTINUATION run must be an uninterrupted frame sequence for the same
    // stream (RFC 9113 §6.10). Appending them contiguously to the single outbound buffer
    // guarantees that ordering (replacing the coroutine writeHeaders' atomic write).
    const std::size_t maxFrame = peerSettings_.maxFrameSize();
    std::size_t offset = 0;
    bool first = true;
    while (offset < headerBlock.size()) {
        const auto chunk = std::min<std::size_t>(headerBlock.size() - offset, maxFrame);
        const bool last = offset + chunk == headerBlock.size();
        const auto flags = static_cast<std::uint8_t>(
            (last ? kHttp2FlagEndHeaders : 0) |
            (first && http2EndsStream(endStream) ? kHttp2FlagEndStream : 0));
        output_.appendFrame(
            first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation,
            flags, stream.id(), headerBlock.substr(offset, chunk));
        offset += chunk;
        first = false;
    }
}

Http2BufferedResponseHeadSubmitResult Http2Connection::submitResponseHead(
    std::uint32_t streamId,
    const HttpResponse& response,
    HttpBufferedResponseWritePlan writePlan) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2BufferedResponseHeadSubmitResult::makeClosedFailure();
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) ||
        stream->localSend().headPending() == nullptr) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    if (writePlan.requestMethod() != stream->requestKnownMethod() ||
        !writePlan.matchesResponse(response)) {
        return Http2BufferedResponseHeadSubmitResult::
            makeResponsePlanMismatchFailure();
    }
    const bool successfulConnect =
        response.status() >= 200 && response.status() < 300 &&
        stream->tunnel().pending() != nullptr;
    if (successfulConnect) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    const auto controlResult = http2FinalResponseControlPlan(response);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2BufferedResponseHeadSubmitResult::
            makeInvalidMessageFailure();
    }

    const auto headPlanResult = http2BufferedResponseHeadPlan(
        writePlan,
        response);
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        const auto error = headPlanResult.failure()->error();
        const bool responsePlanMismatch =
            error == Http2ResponseHeadPlanError::kResponseStatusMismatch ||
            error == Http2ResponseHeadPlanError::
                kResponseRepresentationMismatch;
        return responsePlanMismatch
            ? Http2BufferedResponseHeadSubmitResult::
                makeResponsePlanMismatchFailure()
            : Http2BufferedResponseHeadSubmitResult::
                makeInvalidMessageFailure();
    }
    appendHttp2ResponseHeaders(
        *stream,
        response,
        *headPlan,
        *http2Control);
    const auto endStream = writePlan.sendBody()
        ? Http2EndStream::kKeepOpen
        : Http2EndStream::kEndStream;
    if (headPlan->bodyPlan().bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else {
        stream->beginLocalContentKnownLength(writePlan.contentLength());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        endStream);
    if (http2EndsStream(endStream)) {
        (void)stream->commitLocalHeadEndStream();
    } else {
        (void)stream->beginLocalResponseContent();
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2BufferedResponseHeadSubmitResult::makeSubmitted(
        std::move(writePlan));
}

Http2StreamingResponseHeadSubmitResult Http2Connection::submitStreamingResponseHead(
    std::uint32_t streamId,
    HttpResponse head,
    ResponseStreamKind kind,
    ResponseTrailerIntent trailerIntent) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2StreamingResponseHeadSubmitResult::makeClosedFailure();
    }
    const bool successfulConnect =
        head.status() >= 200 && head.status() < 300 &&
        stream->tunnel().pending() != nullptr;
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) ||
        stream->localSend().headPending() == nullptr || successfulConnect) {
        return Http2StreamingResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    const auto controlResult = http2FinalResponseControlPlan(head);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2StreamingResponseHeadSubmitResult::
            makeInvalidMessageFailure();
    }
    auto preparedCommitPlan = httpResponseStreamCommitPlan(
        ResponseStreamFraming::kHttp2Frames,
        stream->requestKnownMethod(),
        head.status(),
        trailerIntent);
    auto streamHead = prepareResponseStreamHead(
        std::move(head),
        kind,
        std::move(preparedCommitPlan));
    const auto& commitPlan = streamHead.commitPlan();
    // One prepared plan owns both the encoded Content-Length metadata and the
    // local DATA accounting contract. Explicit length is parsed exactly once;
    // absence remains unbounded, while content-forbidden responses never become
    // DATA-open.
    const auto headPlanResult = http2StreamingResponseHeadPlan(
        commitPlan.bodyPlan(),
        streamHead.response());
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        return Http2StreamingResponseHeadSubmitResult::
            makeInvalidMessageFailure();
    }
    appendHttp2ResponseHeaders(
        *stream,
        streamHead.response(),
        *headPlan,
        *http2Control);
    const auto endStream =
        commitPlan.headDisposition() == ResponseStreamHeadDisposition::kMessageEnded
        ? Http2EndStream::kEndStream
        : Http2EndStream::kKeepOpen;
    if (headPlan->bodyPlan().bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else if (const auto contentLength =
                   headPlan->streamingContentLength()) {
        stream->beginLocalContentKnownLength(*contentLength);
    } else {
        stream->beginLocalContentUnbounded();
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        endStream);
    if (commitPlan.headDisposition() ==
        ResponseStreamHeadDisposition::kTrailersOnly) {
        (void)stream->beginLocalResponseTrailersOnly();
    } else {
        if (http2EndsStream(endStream)) {
            (void)stream->commitLocalHeadEndStream();
        } else {
            (void)stream->beginLocalResponseContent();
        }
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2StreamingResponseHeadSubmitResult::makeSubmitted(commitPlan);
}

Http2SubmitStatus Http2Connection::submitInterimResponseHead(
    std::uint32_t streamId,
    const HttpInterimResponseHead& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) ||
        stream->localSend().headPending() == nullptr) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (appendHttp2InterimResponseHeaders(*stream, response) !=
        Http2InterimResponseHeaderEncodeStatus::kOk) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2SubmitStatus::kAccepted;
}

Http2DataSubmitStatus Http2Connection::submitData(
    std::uint32_t streamId,
    std::string_view chunk,
    Http2EndStream endStream) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2DataSubmitStatus::kClosed;
    }
    const auto& localSend = stream->localSend();
    if (localSend.requestContentOpen() == nullptr &&
        localSend.responseContentOpen() == nullptr &&
        localSend.tunnelOpen() == nullptr) {
        return Http2DataSubmitStatus::kInvalidState;
    }
    // One queued submission per stream is the hard backpressure boundary. The
    // current input remains caller-owned and can be retried after the prior one drains.
    for (const auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            return Http2DataSubmitStatus::kBackpressured;
        }
    }
    switch (stream->checkLocalContentAccept(chunk.size(), http2EndsStream(endStream))) {
        case Http2LocalContentCheck::kAccepted:
            break;
        case Http2LocalContentCheck::kNotStarted:
        case Http2LocalContentCheck::kForbidden:
            return Http2DataSubmitStatus::kInvalidState;
        case Http2LocalContentCheck::kLengthExceeded:
            return Http2DataSubmitStatus::kContentLengthExceeded;
        case Http2LocalContentCheck::kLengthIncomplete:
            return Http2DataSubmitStatus::kContentLengthIncomplete;
    }
    // Prepare every allocation needed for a deferred suffix BEFORE accepting the
    // input or consuming flow-control window. A recoverable allocation failure can
    // therefore never leave a framed prefix without its core-owned remainder.
    std::optional<Http2PendingSend> deferred;
    if (!chunk.empty()) {
        const auto immediateBytes = std::min(
            chunk.size(), http2AvailableSendWindow(connectionSendWindow_, *stream));
        if (immediateBytes < chunk.size()) {
            std::pmr::string remainder(resource_);
            remainder.append(
                chunk.data() + immediateBytes, chunk.size() - immediateBytes);
            pendingSends_.reserve(pendingSends_.size() + 1);
            deferred.emplace(Http2PendingSend{
                streamId,
                std::move(remainder),
                0,
                endStream,
                std::pmr::string(resource_)});
        }
    }
    // Accepted means ownership of the WHOLE input, even when flow control below
    // can only materialize a prefix and the prepared suffix becomes pending.
    stream->acceptLocalContent(chunk.size());
    if (chunk.empty()) {
        if (http2EndsStream(endStream)) {
            output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
            (void)stream->commitLocalEndStream();
            releaseLocalRequestStreamIfClosed(*stream);
        }
        return Http2DataSubmitStatus::kAccepted;
    }
    const auto consumed = sendDataUpToWindow(*stream, chunk, 0, endStream);
    if (consumed < chunk.size()) {
        // immediateBytes above is the exact total that sendDataUpToWindow can
        // consume from the current windows, so a deferred value must exist here.
        pendingSends_.push_back(std::move(*deferred));
        if (http2EndsStream(endStream)) {
            (void)stream->queueLocalEndStream();
        }
        return Http2DataSubmitStatus::kQueued;
    }
    if (http2EndsStream(endStream)) {
        (void)stream->commitLocalEndStream();
        releaseLocalRequestStreamIfClosed(*stream);
    }
    return Http2DataSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitConnectResponseHead(
    std::uint32_t streamId,
    const HttpResponse& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) ||
        stream->localSend().headPending() == nullptr ||
        stream->tunnel().pending() == nullptr) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (!http2IsValidConnectResponseHead(response)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    const auto controlResult = http2FinalResponseControlPlan(response);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    const auto headPlanResult = http2ConnectResponseHeadPlan(
        httpResponseBodyPlan(HttpKnownMethod::kConnect, response.status()));
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendHttp2ResponseHeaders(
        *stream,
        response,
        *headPlan,
        *http2Control);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(
            stream->responseHeaderBlock().data(),
            stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    (void)stream->openLocalConnectTunnel();
    http2ReleaseResponseHeaderBlock(*stream);
    if (http2RemotePeerHalfClosed(*stream)) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2SubmitStatus::kAccepted;
}

Http2WebSocketHandshakeSubmitResult
Http2Connection::submitWebSocketHandshake(
    std::uint32_t streamId,
    WebSocketServerNegotiation negotiation) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2WebSocketHandshakeSubmitResult::makeFailure(
            Http2WebSocketHandshakeSubmitError::kClosed);
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) ||
        stream->localSend().headPending() == nullptr ||
        !http2IsPendingWebSocketConnect(*stream)) {
        return Http2WebSocketHandshakeSubmitResult::makeFailure(
            Http2WebSocketHandshakeSubmitError::kInvalidState);
    }
    http2EncodeWebSocketHandshakeHeaders(
        stream->responseHeaderBlock(),
        negotiation);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    (void)stream->openLocalConnectTunnel();
    http2ReleaseResponseHeaderBlock(*stream);
    if (http2RemotePeerHalfClosed(*stream)) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2WebSocketHandshakeSubmitResult::makeSubmitted(negotiation);
}

Http2FinishSubmitStatus Http2Connection::finishResponse(
    std::uint32_t streamId,
    const HttpResponseTrailerSection& trailers) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2FinishSubmitStatus::kClosed;
    }
    if (stream->localSend().responseContentOpen() == nullptr &&
        stream->localSend().responseTrailersOnly() == nullptr) {
        return Http2FinishSubmitStatus::kInvalidState;
    }
    if (!stream->localContent().lengthComplete()) {
        return Http2FinishSubmitStatus::kContentLengthIncomplete;
    }
    if (stream->localSend().responseTrailersOnly() != nullptr &&
        trailers.empty()) {
        // A trailers-only response cannot fall back to DATA(END_STREAM): its
        // method/status explicitly forbids DATA, including an empty terminal frame.
        return Http2FinishSubmitStatus::kInvalidState;
    }
    // The entire semantic trailer section was validated before the initial head
    // commit and is encoded in detached
    // storage before output, pending DATA, or stream phase changes. It either joins
    // the terminal transaction whole or leaves no per-stream staged side channel.
    std::pmr::string trailerBlock(resource_);
    appendHttp2ResponseTrailers(trailerBlock, trailers);
    // If the body still has a window-blocked remainder, the trailer HEADERS must NOT
    // jump ahead of that queued DATA. Stash it on the pending entry and move END_STREAM
    // from the body to the trailer (markSendWindowOpened emits it once the body drains).
    for (auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            if (trailerBlock.empty()) {
                pending.endStream = Http2EndStream::kEndStream;
            } else {
                pending.endStream = Http2EndStream::kKeepOpen;
                pending.trailerBlock.swap(trailerBlock);
            }
            (void)stream->queueLocalEndStream();
            return Http2FinishSubmitStatus::kQueued;
        }
    }
    if (trailerBlock.empty()) {
        output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
        (void)stream->commitLocalEndStream();
        return Http2FinishSubmitStatus::kAccepted;
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(trailerBlock.data(), trailerBlock.size()),
        Http2EndStream::kEndStream);
    (void)stream->commitLocalEndStream();
    return Http2FinishSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitReset(
    std::uint32_t streamId,
    Http2ErrorCode error) {
    if (streamId == 0) {
        return Http2SubmitStatus::kInvalidState;
    }
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return closedStreams_.source(streamId).has_value()
            ? Http2SubmitStatus::kClosed
            : Http2SubmitStatus::kInvalidState;
    }
    if (stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    // A client-created stream is still RFC-idle until its request HEADERS are
    // submitted; RST_STREAM on that state would make the peer close the connection.
    // A server owner does not own a peer stream until its initial header block has
    // decoded; rejecting an early reset also preserves the mandatory CONTINUATION run.
    if ((role_ == Http2Role::kClient &&
         stream->localSend().headPending() != nullptr) ||
        (role_ == Http2Role::kServer && !http2RemoteFinalHeadDecoded(*stream)) ||
        (http2RemotePeerHalfClosed(*stream) &&
         stream->localSend().endStreamCommitted() != nullptr)) {
        return Http2SubmitStatus::kInvalidState;
    }
    output_.appendRstStream(streamId, error);
    return closeStreamByOwner(streamId)
        ? Http2SubmitStatus::kAccepted
        : Http2SubmitStatus::kClosed;
}

}  // namespace ruvia::detail
