// Runtime checks that complement the compile-only HTTP API contract guard.
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ruvia/http/HttpCache.h>
#include <ruvia/http/Cookies.h>
#include <ruvia/http/Hpack.h>
#include <ruvia/http/ProtocolByteLimit.h>
#include <ruvia/http/HttpHeader.h>
#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpClientRedirect.h>
#include <ruvia/http/HttpContentCodec.h>
#include <ruvia/http/HttpContentCoding.h>
#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>
#include <ruvia/http/Http1InterimResponseWriter.h>
#include <ruvia/http/Http1RequestBodyPlan.h>
#include <ruvia/http/Http1RequestParser.h>
#include <ruvia/http/Http2Connection.h>
#include <ruvia/http/Http2Framing.h>
#include <ruvia/http/HttpInterimResponse.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/http/HttpProtocolError.h>
#include <ruvia/http/HttpProtocolVersion.h>
#include <ruvia/http/HttpRequest.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/HttpSetCookie.h>
#include <ruvia/http/HttpExpectations.h>
#include <ruvia/http/HttpTransferCoding.h>
#include <ruvia/http/MultipartParser.h>
#include <ruvia/http/Sse.h>
#include <ruvia/http/UrlEncoding.h>
#include <ruvia/http/WebSocketServerConnection.h>
#include <ruvia/http/WebSocketHandshake.h>
#include <ruvia/http/detail/util/AsciiCase.h>
#include <ruvia/http/detail/util/BorrowedView.h>
#include <ruvia/http/detail/cookie/CookieValidation.h>
#include <ruvia/http/detail/field/HttpMediaType.h>
#include <ruvia/http/detail/field/HttpQualityValue.h>
#include <ruvia/http/detail/field/HeaderTokenUtils.h>
#include <ruvia/http/detail/field/HttpByteRange.h>
#include <ruvia/http/detail/coding/HttpContentCoding.h>
#include <ruvia/http/detail/coding/HttpContentLength.h>
#include <ruvia/http/detail/field/HttpEntityTag.h>
#include <ruvia/http/detail/field/HttpExpectations.h>
#include <ruvia/http/detail/util/HttpOws.h>
#include <ruvia/http/detail/request/HttpRequestBodyFailure.h>
#include <ruvia/http/detail/coding/HttpRequestContentSemantics.h>
#include <ruvia/http/detail/coding/HttpTransferEncoding.h>
#include <ruvia/http/detail/response/HttpResponseBody.h>
#include <ruvia/http/detail/response/HttpResponseBodyAccess.h>
#include <ruvia/http/detail/coding/HttpResponseContentSemantics.h>
#include <ruvia/http/detail/response/HttpResponseFileBody.h>
#include <ruvia/http/detail/request/RequestBodyDecoding.h>
#include <ruvia/http/detail/client/HttpClientContentEncoding.h>
#include <ruvia/http/detail/client/HttpOriginView.h>
#include <ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h>
#include <ruvia/http/detail/http1/Http1ChunkedFraming.h>
#include <ruvia/http/detail/coding/HttpTransferCodingDecoder.h>
#include <ruvia/http/detail/http1/Http1ResponseHeadPlan.h>
#include <ruvia/http/detail/http1/Http1ServerRequestParser.h>
#include <ruvia/http/detail/http1/Http1ServerSemantics.h>
#include <ruvia/http/detail/http2/Http2Connection.h>
#include <ruvia/http/detail/http2/stream/Http2ClosedStreams.h>
#include <ruvia/http/detail/http2/Http2Event.h>
#include <ruvia/http/detail/http2/frame/Http2FramePayload.h>
#include <ruvia/http/detail/http2/hpack/Http2HeaderList.h>
#include <ruvia/http/detail/http2/hpack/Http2Hpack.h>
#include <ruvia/http/detail/http2/message/Http2LocalSendState.h>
#include <ruvia/http/detail/http2/frame/Http2OutputBuffer.h>
#include <ruvia/http/detail/http2/settings/Http2PeerSettings.h>
#include <ruvia/http/detail/http2/frame/Http2PayloadSlice.h>
#include <ruvia/http/detail/http2/message/Http2RemoteContentState.h>
#include <ruvia/http/detail/http2/message/Http2RemoteReceiveState.h>
#include <ruvia/http/detail/http2/message/Http2RequestBuilder.h>
#include <ruvia/http/detail/http2/message/Http2ResponseHeadPlan.h>
#include <ruvia/http/detail/http2/stream/Http2StreamHeaderBlocks.h>
#include <ruvia/http/detail/http2/stream/Http2StreamLifecycle.h>
#include <ruvia/http/detail/http2/stream/Http2StreamRequestData.h>
#include <ruvia/http/detail/http2/stream/Http2StreamRequestState.h>
#include <ruvia/http/detail/http2/stream/Http2StreamState.h>
#include <ruvia/http/detail/http2/stream/Http2StreamTable.h>
#include <ruvia/http/detail/http2/stream/Http2TunnelState.h>
#include <ruvia/http/detail/parser/MultipartPartAccess.h>
#include <ruvia/http/detail/parser/MimeFieldGrammar.h>
#include <ruvia/http/detail/parser/MultipartDelimiter.h>
#include <ruvia/http/detail/parser/MultipartPartHeaders.h>
#include <ruvia/http/detail/parser/MultipartStreamPartAccess.h>
#include <ruvia/http/detail/cookie/SetCookiePlan.h>
#include <ruvia/http/detail/parser/HttpChunkParser.h>
#include <ruvia/http/detail/parser/HttpHeaderBlockParser.h>
#include <ruvia/http/detail/parser/HttpRequestTarget.h>
#include <ruvia/http/detail/server/HttpFinalResponseControlPlan.h>
#include <ruvia/http/detail/server/HttpResponseHeadBuffer.h>
#include <ruvia/http/detail/server/HttpResponseWritePlan.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h>
#include <ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketClosePayload.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameReader.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketFrameView.h>
#include <ruvia/http/detail/websocket/message/HttpWebSocketInboundAssembler.h>
#include <ruvia/http/detail/websocket/frame/HttpWebSocketPayloadValidation.h>
#include <ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h>
#include <ruvia/http/detail/websocket/WsConnection.h>
#include <ruvia/http/detail/websocket/WsEvent.h>

int main() {
    std::pmr::monotonic_buffer_resource publicProtocolResource;
    std::pmr::string hpackBlock(&publicProtocolResource);
    ruvia::HpackEncoder::encodeHeader(hpackBlock, "x-public", "yes");
    std::pair<std::string, std::string> decodedHeader;
    ruvia::HpackDecoder hpackDecoder({.resource = &publicProtocolResource});
    const auto hpackResult =
        hpackDecoder.decode(hpackBlock, [&](std::string_view name, std::string_view value) {
            decodedHeader.first.assign(name);
            decodedHeader.second.assign(value);
            return true;
        });
    if (!hpackResult.decoded() || decodedHeader.first != "x-public" ||
        decodedHeader.second != "yes") {
        return 60;
    }
    const std::array<char, 5> indexedLiteral{static_cast<char>(0x40), 1, 'x', 1, 'y'};
    bool callbackExceptionObserved = false;
    try {
        (void)hpackDecoder.decode(std::string_view(indexedLiteral.data(), indexedLiteral.size()),
            [](std::string_view, std::string_view) -> bool {
                throw std::runtime_error("callback failure");
            });
    } catch (const std::runtime_error&) {
        callbackExceptionObserved = true;
    }
    std::pair<std::string, std::string> dynamicHeader;
    const char dynamicIndex = static_cast<char>(0xbe);  // first dynamic entry, index 62
    const auto dynamicResult = hpackDecoder.decode(
        std::string_view(&dynamicIndex, 1), [&](std::string_view name, std::string_view value) {
            dynamicHeader = {std::string(name), std::string(value)};
            return true;
        });
    if (!callbackExceptionObserved || !dynamicResult.decoded() ||
        dynamicHeader != std::pair(std::string("x"), std::string("y"))) {
        return 60;
    }

    std::array<char, ruvia::kHttp2FrameHeaderBytes> publicFrameHeader{};
    if (!ruvia::encodeHttp2FrameHeader(publicFrameHeader, 7, ruvia::Http2FrameType::kData, 1, 3)) {
        return 60;
    }
    const auto parsedPublicFrame = ruvia::parseHttp2FrameHeader(publicFrameHeader);
    if (!parsedPublicFrame || parsedPublicFrame->length != 7 ||
        parsedPublicFrame->type != static_cast<std::uint8_t>(ruvia::Http2FrameType::kData) ||
        parsedPublicFrame->flags != 1 || parsedPublicFrame->streamId != 3) {
        return 60;
    }

    auto publicHttp2 = ruvia::Http2Connection::client({.resource = &publicProtocolResource});
    if (!publicHttp2.pendingOutput().starts_with(ruvia::kHttp2ClientPreface)) {
        return 60;
    }
    (void)publicHttp2.consumeOutput(publicHttp2.pendingOutput().size());
    const auto publicRequest = publicHttp2.submitRequestHead(ruvia::Http2RegularRequestHeadView{
        .method = "GET", .scheme = "https", .authority = "example.test", .target = "/"});
    const auto* publicSubmitted = publicRequest.submitted();
    if (publicSubmitted == nullptr || publicRequest.failure() != nullptr) {
        return 60;
    }
    (void)publicHttp2.consumeOutput(publicHttp2.pendingOutput().size());

    std::pmr::string publicPeerBytes(&publicProtocolResource);
    std::array<char, ruvia::kHttp2FrameHeaderBytes> settingsHeader{};
    (void)ruvia::encodeHttp2FrameHeader(settingsHeader, 0, ruvia::Http2FrameType::kSettings, 0, 0);
    publicPeerBytes.append(settingsHeader.data(), settingsHeader.size());
    std::pmr::string finalHead(&publicProtocolResource);
    ruvia::HpackEncoder::encodeStatus(finalHead, ruvia::http_status::kNoContent);
    std::array<char, ruvia::kHttp2FrameHeaderBytes> responseHeader{};
    (void)ruvia::encodeHttp2FrameHeader(responseHeader,
        static_cast<std::uint32_t>(finalHead.size()), ruvia::Http2FrameType::kHeaders, 0x5,
        publicSubmitted->streamId());
    publicPeerBytes.append(responseHeader.data(), responseHeader.size());
    publicPeerBytes.append(finalHead.data(), finalHead.size());
    if (publicHttp2.feed(publicPeerBytes) != ruvia::Http2FeedResult::kAccepted) {
        return 60;
    }
    const auto publicResponseHead = publicHttp2.nextEvent();
    const auto publicResponseEnd = publicHttp2.nextEvent();
    const auto* responseHead = publicResponseHead ? publicResponseHead->responseHead() : nullptr;
    if (responseHead == nullptr ||
        responseHead->head().status() != ruvia::http_status::kNoContent || !publicResponseEnd ||
        publicResponseEnd->messageEnd() == nullptr) {
        return 60;
    }

    auto publicResetClient = ruvia::Http2Connection::client({.resource = &publicProtocolResource});
    (void)publicResetClient.consumeOutput(publicResetClient.pendingOutput().size());
    for (std::size_t index = 0; index < 256; ++index) {
        const auto submitted =
            publicResetClient.submitRequestHead(ruvia::Http2RegularRequestHeadView{.method = "GET",
                .scheme = "https",
                .authority = "example.test",
                .target = "/cancelled"});
        const auto* accepted = submitted.submitted();
        if (accepted == nullptr || submitted.failure() != nullptr) {
            return 61;
        }
        (void)publicResetClient.consumeOutput(publicResetClient.pendingOutput().size());
        if (publicResetClient.submitReset(accepted->streamId(), ruvia::Http2ErrorCode::kCancel) !=
            ruvia::Http2SubmitStatus::kAccepted) {
            return 61;
        }
        (void)publicResetClient.consumeOutput(publicResetClient.pendingOutput().size());
    }

    auto publicConnectClient =
        ruvia::Http2Connection::client({.resource = &publicProtocolResource});
    (void)publicConnectClient.consumeOutput(publicConnectClient.pendingOutput().size());
    const auto publicConnect = publicConnectClient.submitRequestHead(
        ruvia::Http2ConnectRequestHeadView{.authority = "example.test:443"});
    if (publicConnect.submitted() == nullptr) return 63;
    (void)publicConnectClient.submitReset(
        publicConnect.submitted()->streamId(), ruvia::Http2ErrorCode::kCancel);

    auto publicExtendedConnectClient =
        ruvia::Http2Connection::client({.resource = &publicProtocolResource});
    (void)publicExtendedConnectClient.consumeOutput(
        publicExtendedConnectClient.pendingOutput().size());
    const auto unavailableExtendedConnect = publicExtendedConnectClient.submitRequestHead(
        ruvia::Http2ExtendedConnectRequestHeadView{.protocol = "websocket",
            .scheme = "https",
            .authority = "example.test",
            .target = "/ws"});
    if (unavailableExtendedConnect.failure() == nullptr ||
        unavailableExtendedConnect.failure()->error() !=
            ruvia::Http2RequestHeadSubmitError::kPeerCapabilityUnavailable)
        return 64;
    constexpr std::array<char, 6> enableConnectProtocol{0, 8, 0, 0, 0, 1};
    static_assert(enableConnectProtocol.size() <= (std::numeric_limits<std::uint32_t>::max)());
    std::array<char, ruvia::kHttp2FrameHeaderBytes> enableConnectHeader{};
    (void)ruvia::encodeHttp2FrameHeader(enableConnectHeader,
        static_cast<std::uint32_t>(enableConnectProtocol.size()), ruvia::Http2FrameType::kSettings,
        0, 0);
    std::pmr::string enableConnectWire(&publicProtocolResource);
    enableConnectWire.append(enableConnectHeader.data(), enableConnectHeader.size());
    enableConnectWire.append(enableConnectProtocol.data(), enableConnectProtocol.size());
    if (publicExtendedConnectClient.feed(enableConnectWire) != ruvia::Http2FeedResult::kAccepted)
        return 65;
    const auto publicExtendedConnect = publicExtendedConnectClient.submitRequestHead(
        ruvia::Http2ExtendedConnectRequestHeadView{.protocol = "custom",
            .scheme = "https",
            .authority = "example.test",
            .target = "/tunnel"});
    if (publicExtendedConnect.submitted() == nullptr) return 66;
    (void)publicExtendedConnectClient.submitReset(
        publicExtendedConnect.submitted()->streamId(), ruvia::Http2ErrorCode::kCancel);

    auto publicServer = ruvia::Http2Connection::server({.resource = &publicProtocolResource});
    (void)publicServer.consumeOutput(publicServer.pendingOutput().size());
    std::pmr::string publicRequestHead(&publicProtocolResource);
    ruvia::HpackEncoder::encodeHeader(publicRequestHead, ":method", "POST");
    ruvia::HpackEncoder::encodeHeader(publicRequestHead, ":scheme", "https");
    ruvia::HpackEncoder::encodeHeader(publicRequestHead, ":authority", "example.test");
    ruvia::HpackEncoder::encodeHeader(publicRequestHead, ":path", "/upload");
    ruvia::HpackEncoder::encodeHeader(publicRequestHead, "content-length", "2");
    std::pmr::string publicRequestWire(ruvia::kHttp2ClientPreface, &publicProtocolResource);
    const auto appendPublicFrame = [&](ruvia::Http2FrameType type, std::uint8_t flags,
                                       std::uint32_t streamId, std::string_view payload) {
        std::array<char, ruvia::kHttp2FrameHeaderBytes> header{};
        (void)ruvia::encodeHttp2FrameHeader(
            header, static_cast<std::uint32_t>(payload.size()), type, flags, streamId);
        publicRequestWire.append(header.data(), header.size());
        publicRequestWire.append(payload);
    };
    appendPublicFrame(ruvia::Http2FrameType::kSettings, 0, 0, {});
    appendPublicFrame(ruvia::Http2FrameType::kHeaders, 0x4, 1, publicRequestHead);
    appendPublicFrame(ruvia::Http2FrameType::kData, 0, 1, "a");
    appendPublicFrame(ruvia::Http2FrameType::kData, 0x1, 1, "b");
    if (publicServer.feed(publicRequestWire) != ruvia::Http2FeedResult::kAccepted) return 62;
    auto publicRequestEvent = publicServer.nextEvent();
    auto firstChunkEvent = publicServer.nextEvent();
    auto secondChunkEvent = publicServer.nextEvent();
    auto publicRequestEnd = publicServer.nextEvent();
    auto* publicRequestHeadEvent = publicRequestEvent ? publicRequestEvent->requestHead() : nullptr;
    auto* firstChunk = firstChunkEvent ? firstChunkEvent->messageBodyChunk() : nullptr;
    auto* secondChunk = secondChunkEvent ? secondChunkEvent->messageBodyChunk() : nullptr;
    if (publicRequestHeadEvent == nullptr || publicRequestHeadEvent->request().method() != "POST" ||
        firstChunk == nullptr || firstChunk->bytes() != "a" || secondChunk == nullptr ||
        secondChunk->bytes() != "b" || !publicRequestEnd ||
        publicRequestEnd->messageEnd() == nullptr)
        return 62;

    auto otherPublicServer = ruvia::Http2Connection::server({.resource = &publicProtocolResource});
    (void)otherPublicServer.consumeOutput(otherPublicServer.pendingOutput().size());
    if (otherPublicServer.feed(publicRequestWire) != ruvia::Http2FeedResult::kAccepted) return 62;
    auto otherRequestEvent = otherPublicServer.nextEvent();
    auto otherFirstChunkEvent = otherPublicServer.nextEvent();
    auto otherSecondChunkEvent = otherPublicServer.nextEvent();
    (void)otherPublicServer.nextEvent();
    auto* otherRequestHead = otherRequestEvent ? otherRequestEvent->requestHead() : nullptr;
    auto* otherFirstChunk =
        otherFirstChunkEvent ? otherFirstChunkEvent->messageBodyChunk() : nullptr;
    auto* otherSecondChunk =
        otherSecondChunkEvent ? otherSecondChunkEvent->messageBodyChunk() : nullptr;
    if (otherRequestHead == nullptr || otherFirstChunk == nullptr || otherSecondChunk == nullptr)
        return 62;
    auto foreignCredit = otherFirstChunk->takeCredit();
    if (publicServer.acknowledge(std::move(foreignCredit)) !=
            ruvia::Http2ReceivedDataAcknowledgeStatus::kInvalidCredit ||
        otherPublicServer.acknowledge(std::move(foreignCredit)) !=
            ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged ||
        otherPublicServer.acknowledge(otherSecondChunk->takeCredit()) !=
            ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged)
        return 62;
    if (publicServer.release(std::move(*otherRequestHead)) !=
            ruvia::Http2ServerRequestReleaseStatus::kInvalidLease ||
        otherPublicServer.release(std::move(*otherRequestHead)) !=
            ruvia::Http2ServerRequestReleaseStatus::kReleased)
        return 62;

    auto firstCredit = firstChunk->takeCredit();
    if (publicServer.acknowledge(std::move(firstCredit)) !=
            ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged ||
        publicServer.acknowledge(std::move(firstCredit)) !=
            ruvia::Http2ReceivedDataAcknowledgeStatus::kInvalidCredit)
        return 62;
    if (publicServer.acknowledge(secondChunk->takeCredit()) !=
        ruvia::Http2ReceivedDataAcknowledgeStatus::kAcknowledged)
        return 62;
    if (publicServer.release(std::move(*publicRequestHeadEvent)) !=
            ruvia::Http2ServerRequestReleaseStatus::kReleased ||
        publicServer.release(std::move(*publicRequestHeadEvent)) !=
            ruvia::Http2ServerRequestReleaseStatus::kInvalidLease)
        return 62;
    if (!publicRequestHeadEvent->request().method().empty()) return 62;

    bool invalidWebSocketCompressionRejected = false;
    try {
        ruvia::WebSocketServerConnectionOptions invalidOptions{.resource = &publicProtocolResource};
        invalidOptions.compression = static_cast<ruvia::WebSocketCompression>(255);
        ruvia::WebSocketServerConnection invalidWebSocket(invalidOptions);
    } catch (const std::invalid_argument&) {
        invalidWebSocketCompressionRejected = true;
    }
    if (!invalidWebSocketCompressionRejected) return 60;

    ruvia::WebSocketServerConnection publicWebSocket({.resource = &publicProtocolResource});
    constexpr std::array<unsigned char, 6> websocketPayload{'p', 'u', 'b', 'l', 'i', 'c'};
    constexpr std::array<unsigned char, 4> websocketMask{1, 2, 3, 4};
    std::array<char, 12> websocketFrame{};
    websocketFrame[0] = static_cast<char>(0x81);
    websocketFrame[1] = static_cast<char>(0x80 | websocketPayload.size());
    for (std::size_t index = 0; index < websocketMask.size(); ++index)
        websocketFrame[2 + index] = static_cast<char>(websocketMask[index]);
    for (std::size_t index = 0; index < websocketPayload.size(); ++index)
        websocketFrame[6 + index] = static_cast<char>(
            websocketPayload[index] ^ websocketMask[index % websocketMask.size()]);
    if (publicWebSocket.feed(std::string_view(websocketFrame.data(), websocketFrame.size())) !=
        ruvia::WebSocketFeedStatus::kAccepted)
        return 60;
    const auto websocketEvent = publicWebSocket.nextEvent();
    if (!websocketEvent || websocketEvent->message() == nullptr ||
        websocketEvent->message()->payload() != "public")
        return 60;
    if (publicWebSocket.submitFrame(ruvia::WebSocketOpcode::kText, "public") !=
            ruvia::WebSocketFrameSubmitStatus::kAccepted ||
        publicWebSocket.outputPlan().bytes().empty()) {
        return 60;
    }

    const auto sseFrame = ruvia::formatSseMessage(
        ruvia::SseMessage{.data = "public", .retry = std::chrono::milliseconds::zero()},
        {.resource = &publicProtocolResource});
    bool negativeRetryRejected = false;
    try {
        (void)ruvia::formatSseMessage(ruvia::SseMessage{.retry = std::chrono::milliseconds{-1}},
            {.resource = &publicProtocolResource});
    } catch (const std::invalid_argument&) {
        negativeRetryRejected = true;
    }
    if (sseFrame != "retry: 0\ndata: public\n\n" || !negativeRetryRejected) {
        return 60;
    }

    ruvia::CacheControlFieldParser cacheControlParser;
    cacheControlParser.update("public, max-age=invalid");
    cacheControlParser.update("no-transform, max-age=60");
    const auto installedCacheControl = cacheControlParser.finish();
    if (!installedCacheControl.has(ruvia::CacheControlDirective::kPublic) ||
        !installedCacheControl.has(ruvia::CacheControlDirective::kNoTransform) ||
        installedCacheControl.maxAge().has_value()) {
        return 54;
    }
    if (!ruvia::detail::isValidCookieAttribute("/a path") ||
        ruvia::detail::isValidCookieAttribute("/a\tpath") ||
        ruvia::detail::isValidCookieAttribute("/caf\xc3\xa9") ||
        !ruvia::detail::cookieNameStartsWithIgnoreCase("__sEcUrE-id", "__Secure-")) {
        return 53;
    }
    ruvia::CookieOptions cookieOptions;
    cookieOptions.sameSite = ruvia::CookieSameSite::kLax;
    cookieOptions.maxAge = std::chrono::seconds(60);
    const ruvia::detail::SetCookiePlan cookiePlan("sid", "value", cookieOptions);
    std::array<char, 128> cookieBuffer{};
    cookiePlan.write(cookieBuffer.data());
    if (std::string_view(cookieBuffer.data(), cookiePlan.size()) !=
        "sid=value; Path=/; Max-Age=60; SameSite=Lax") {
        return 52;
    }

    const auto decodedUrl = ruvia::detail::decodeUrlComponent(
        "installed%20decoder", {.mode = ruvia::detail::UrlDecodeMode::kPercent,
                                   .resource = std::pmr::get_default_resource()});
    const auto malformedUrl = ruvia::detail::decodeUrlComponent(
        "prefix%2", {.mode = ruvia::detail::UrlDecodeMode::kPercent,
                        .resource = std::pmr::get_default_resource()});
    if (!decodedUrl.has_value() || std::string_view(*decodedUrl) != "installed decoder" ||
        malformedUrl.has_value()) {
        return 51;
    }
    auto encodedContent =
        ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, "installed content encoder",
            {.maxEncodedBytes = 1024, .resource = std::pmr::get_default_resource()});
    if (encodedContent.encoded() == nullptr || encodedContent.failure() != nullptr ||
        encodedContent.encoded()->bytes().empty()) {
        return 50;
    }
    const auto identityDecode = ruvia::decodeHttpContent(ruvia::HttpContentCoding::kIdentity,
        "identity", {.maxDecodedBytes = 8, .resource = std::pmr::get_default_resource()});
    if (identityDecode.decoded() == nullptr || identityDecode.failure() != nullptr ||
        identityDecode.decoded()->bytes() != "identity") {
        return 49;
    }
    const ruvia::HttpResponse emptyResponse;
    const auto& emptyBody = ruvia::detail::responseBody(emptyResponse);
    if (emptyBody.empty() == nullptr || emptyBody.borrowedBytes() != nullptr ||
        emptyBody.staticBytes() != nullptr || emptyBody.ownedBytes() != nullptr ||
        emptyBody.ownedFile() != nullptr || emptyBody.borrowedFile() != nullptr ||
        emptyBody.file().has_value() || !emptyBody.bytes().empty()) {
        return 48;
    }
    std::pmr::monotonic_buffer_resource remoteReceiveResource;
    ruvia::detail::Http2StreamState remoteReceiveStream(3, &remoteReceiveResource);
    const auto& remoteReceive = remoteReceiveStream.remoteReceive();
    if (remoteReceive.headPending() == nullptr || !remoteReceiveStream.beginStandardConnect() ||
        !remoteReceiveStream.finalizeRemoteConnectHead() ||
        remoteReceive.connectPending() == nullptr || !remoteReceiveStream.rejectConnect() ||
        remoteReceive.connectRejectedAwaitingEndStream() == nullptr ||
        !remoteReceiveStream.finishRemoteRejectedConnect() ||
        remoteReceive.endStream() == nullptr) {
        return 39;
    }
    ruvia::detail::Http2StreamState remotePendingEndStream(5, &remoteReceiveResource);
    const auto& pendingEnd = remotePendingEndStream.remoteReceive();
    if (!remotePendingEndStream.beginStandardConnect() ||
        !remotePendingEndStream.finalizeRemoteConnectHead() ||
        !remotePendingEndStream.finishRemotePendingConnect() ||
        pendingEnd.connectPendingEndStream() == nullptr ||
        !remotePendingEndStream.acceptConnect() || pendingEnd.endStream() == nullptr) {
        return 40;
    }

    std::pmr::monotonic_buffer_resource localSendResource;
    ruvia::detail::Http2StreamState localSendStream(1, &localSendResource);
    const auto& localSend = localSendStream.localSend();
    if (localSend.headPending() == nullptr || localSend.responseTrailersOnly() != nullptr ||
        !localSendStream.beginLocalResponseTrailersOnly() || localSend.headPending() != nullptr ||
        localSend.responseTrailersOnly() == nullptr || !localSendStream.queueLocalEndStream() ||
        localSend.endStreamQueued() == nullptr || !localSendStream.commitLocalEndStream() ||
        localSend.endStreamCommitted() == nullptr ||
        !localSendStream.abort(ruvia::detail::Http2StreamCloseSource::kPeer) ||
        localSend.aborted() == nullptr ||
        localSend.aborted()->source() != ruvia::detail::Http2StreamCloseSource::kPeer ||
        localSendStream.abort(static_cast<ruvia::detail::Http2StreamCloseSource>(0xFF))) {
        return 38;
    }

    ruvia::detail::Http2TunnelState tunnel;
    if (tunnel.notConnect() == nullptr || tunnel.pending() != nullptr ||
        !tunnel.begin(ruvia::detail::Http2ConnectForm::kExtended) ||
        tunnel.notConnect() != nullptr || tunnel.pending() == nullptr ||
        tunnel.pending()->form() != ruvia::detail::Http2ConnectForm::kExtended ||
        !tunnel.accept() || tunnel.pending() != nullptr || tunnel.open() == nullptr ||
        tunnel.rejected() != nullptr) {
        return 37;
    }

    ruvia::detail::Http2RemoteContentState remoteContent;
    if (remoteContent.allowedWithoutLength() == nullptr ||
        remoteContent.allowedKnownLength() != nullptr ||
        remoteContent.allowedWithoutLength()->receivedBytes() != 0 ||
        !remoteContent.declareKnownLength(3) || remoteContent.allowedKnownLength() == nullptr ||
        remoteContent.allowedKnownLength()->declaredLength() != 3 ||
        remoteContent.account(2) != ruvia::detail::Http2RemoteContentAccountingResult::kAccepted) {
        return 36;
    }
    if (remoteContent.terminalLengthValid() ||
        remoteContent.account(2) !=
            ruvia::detail::Http2RemoteContentAccountingResult::kDeclaredLengthExceeded ||
        remoteContent.allowedKnownLength()->receivedBytes() != 2) {
        return 36;
    }
    ruvia::detail::Http2RemoteContentState metadataOnly;
    if (!metadataOnly.declareKnownLength(9) || !metadataOnly.selectMetadataOnly() ||
        metadataOnly.metadataOnlyKnownLength() == nullptr ||
        metadataOnly.metadataOnlyKnownLength()->declaredLength() != 9 ||
        metadataOnly.account(1) !=
            ruvia::detail::Http2RemoteContentAccountingResult::kContentForbidden) {
        return 36;
    }

    static_assert(ruvia::detail::httpResponseContentSemantics(
                      ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk) ==
                  ruvia::detail::HttpResponseContentSemantics::kWithoutContent);
    static_assert(ruvia::detail::httpResponseContentSemantics(
                      ruvia::HttpKnownMethod::kConnect, ruvia::http_status::kOk) ==
                  ruvia::detail::HttpResponseContentSemantics::kConnectTunnel);

    const auto outboundOrigin = ruvia::HttpOriginView::https({.host = "example.test"});
    ruvia::HttpClientRequestView outboundRequest;
    outboundRequest.method = "POST";
    outboundRequest.target = "/submit";
    outboundRequest.content = ruvia::HttpClientRequestContentView::bytes("payload");
    const auto* outboundBytes = outboundRequest.content.borrowedBytes();
    if (outboundRequest.content.withoutContent() != nullptr || outboundBytes == nullptr ||
        outboundBytes->value() != "payload") {
        return 34;
    }
    std::array<char, 512> outboundHeadBuffer;
    const auto outboundPrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, outboundRequest, outboundHeadBuffer);
    const auto* outboundWire = outboundPrepared.prepared();
    const auto* outboundImmediate =
        outboundWire == nullptr ? nullptr : outboundWire->contentPlan().immediate();
    if (outboundOrigin.scheme() != ruvia::HttpScheme::kHttps ||
        outboundOrigin.host() != "example.test" || outboundOrigin.port() != 443 ||
        outboundRequest.target != "/submit" || outboundWire == nullptr ||
        outboundWire->head().find("Content-Length: 7\r\n") == std::string_view::npos ||
        outboundImmediate == nullptr || outboundImmediate->bytes() != "payload") {
        return 17;
    }
    std::array<char, 512> expectHeadBuffer;
    const auto expectPrepared =
        ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, outboundRequest, expectHeadBuffer,
            ruvia::Http1ClientRequestWirePolicy{
                .expectation = ruvia::HttpClientRequestExpectation::kContinue});
    const auto* expectWire = expectPrepared.prepared();
    if (expectWire == nullptr || expectWire->contentPlan().continueGated() == nullptr ||
        expectWire->head().find("Expect: 100-continue\r\n") == std::string_view::npos) {
        return 25;
    }
    ruvia::Http1ClientResponseParser expectParser(expectWire->exchangeState());
    const auto continueResult = expectParser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    const auto* continueHead = continueResult.parsed();
    if (continueHead == nullptr || continueHead->plan().informational() == nullptr ||
        continueHead->plan().informational()->persistence() !=
            ruvia::Http1ClosePolicy::kAllowReuse ||
        continueHead->head().protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 ||
        continueHead->plan().requestContentSignal() !=
            ruvia::HttpClientRequestContentSignal::kContinue) {
        return 25;
    }
    if (expectParser.completeRequestContent() !=
        ruvia::Http1ClientRequestContentCompletionStatus::kCompleted) {
        return 25;
    }
    const auto expectFinalResult = expectParser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    const auto* expectFinalHead = expectFinalResult.parsed();
    if (expectFinalHead == nullptr || expectFinalHead->plan().withoutContent() == nullptr ||
        expectFinalHead->plan().requestContentSignal().has_value()) {
        return 25;
    }
    const auto zeroPortOrigin = ruvia::HttpOriginView::http({.host = "example.test", .port = 0});
    const auto zeroPortAuthority =
        ruvia::detail::makeHttpOriginAuthority(zeroPortOrigin, std::pmr::get_default_resource());
    if (zeroPortAuthority != "example.test:0") {
        return 20;
    }
    bool invalidOriginRejected = false;
    try {
        (void)ruvia::HttpOriginView::http({.host = ""});
    } catch (const std::invalid_argument&) {
        invalidOriginRejected = true;
    }
    if (!invalidOriginRejected) {
        return 21;
    }

    const auto redirectRequestPlan = ruvia::planHttpClientRedirectRequest(
        outboundRequest, {.status = ruvia::http_status::kTemporaryRedirect,
                             .resource = std::pmr::get_default_resource()});
    if (redirectRequestPlan.method() != "POST" ||
        redirectRequestPlan.contentDisposition() !=
            ruvia::HttpClientRedirectContentDisposition::kPreserve) {
        return 38;
    }

    const auto redirectTarget = ruvia::resolveHttpClientRedirectTarget(
        outboundOrigin, {.currentTarget = "/base/current",
                            .location = "../next?x=1",
                            .resource = std::pmr::get_default_resource()});
    if (redirectTarget.resolved() == nullptr || redirectTarget.failure() != nullptr ||
        redirectTarget.resolved()->crossOrigin() ||
        redirectTarget.resolved()->target() != "/next?x=1") {
        return 28;
    }
    const auto crossOrigin = ruvia::resolveHttpClientRedirectTarget(
        outboundOrigin, {.currentTarget = "/base/current",
                            .location = "https://other.test/next",
                            .resource = std::pmr::get_default_resource()});
    if (crossOrigin.resolved() == nullptr || crossOrigin.failure() != nullptr ||
        !crossOrigin.resolved()->crossOrigin()) {
        return 29;
    }

    const ruvia::HttpProtocolError error(ruvia::http_status::kBadRequest, "bad request");
    if (error.status() != ruvia::http_status::kBadRequest) {
        return 2;
    }
    std::pmr::string wsInput(std::pmr::get_default_resource());
    std::size_t wsOffset = 0;
    std::size_t wsPendingCompactUntil = 0;
    const auto wsNeedInput = ruvia::detail::webSocketTryReadFrame(
        wsInput, wsOffset, wsPendingCompactUntil, ruvia::ProtocolByteLimit::limited(1024), false);
    if (wsNeedInput.needInput() == nullptr || wsNeedInput.frame() != nullptr ||
        wsNeedInput.failure() != nullptr) {
        return 31;
    }
    const auto multipart =
        ruvia::parseMultipartBody("--x--\r\n", {.boundary = ruvia::MultipartBoundary("x")});
    if (multipart.failure() != nullptr || multipart.body() == nullptr ||
        !multipart.body()->parts().empty()) {
        return 3;
    }
    ruvia::MultipartParser multipartParser(
        {.boundary = ruvia::MultipartBoundary("x"), .resource = std::pmr::get_default_resource()});
    multipartParser.feed("--x--");
    const auto multipartNeedInput = multipartParser.poll();
    if (multipartNeedInput.needInput() == nullptr) {
        return 18;
    }
    multipartParser.finishInput();
    const auto multipartDone = multipartParser.poll();
    if (multipartDone.done() == nullptr) {
        return 19;
    }
    ruvia::MultipartParser failedMultipartParser(
        {.boundary = ruvia::MultipartBoundary("x"), .resource = std::pmr::get_default_resource()});
    failedMultipartParser.feed(std::string(64 * 1024 + 1, 'p'));
    const auto multipartFailure = failedMultipartParser.poll();
    const auto repeatedMultipartFailure = failedMultipartParser.poll();
    if (multipartFailure.failure() == nullptr || repeatedMultipartFailure.failure() == nullptr ||
        multipartFailure.failure()->protocolError().status() !=
            ruvia::http_status::kContentTooLarge ||
        std::string_view(repeatedMultipartFailure.failure()->protocolError().what()) !=
            multipartFailure.failure()->protocolError().what()) {
        return 72;
    }
    try {
        failedMultipartParser.feed("--x--");
        return 73;
    } catch (const std::logic_error&) {
    }
    constexpr std::string_view chunkedRequest =
        "POST / HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n";
    const auto parseResult = ruvia::Http1RequestParser().parse(chunkedRequest);
    const auto* parsed = parseResult.parsed();
    if (parsed == nullptr || parsed->bodyPlan().chunked() == nullptr ||
        parsed->request().protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 ||
        parsed->wireBody() != "1\r\nx\r\n0\r\n\r\n" ||
        parsed->consumedBytes() != chunkedRequest.size()) {
        return 1;
    }

    ruvia::detail::Http1ServerRequestParser serverParser;
    const auto extensionMethod =
        serverParser.parseMessage("PROPFIND /dav HTTP/1.1\r\nHost: example.test\r\n\r\n");
    if (!extensionMethod.messageReady() || extensionMethod.request.method() != "PROPFIND" ||
        extensionMethod.request.knownMethod() != ruvia::HttpKnownMethod::kUnknown) {
        return 22;
    }
    const auto transferCoded = serverParser.parseMessage(
        "POST / HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n");
    const auto* transferCodedBody = transferCoded.bodyPlan.chunked();
    if (!transferCoded.messageReady() || transferCodedBody == nullptr ||
        transferCodedBody->transferCodings().count != 1 ||
        transferCodedBody->transferCodings().values[0] != ruvia::HttpTransferCoding::kGzip) {
        return 14;
    }

    ruvia::HttpClientRequestView getRequest;
    getRequest.method = "GET";
    std::array<char, 256> getHeadBuffer;
    const auto getPrepared =
        ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, getRequest, getHeadBuffer);
    const auto* getWire = getPrepared.prepared();
    constexpr std::string_view closeDelimitedHead = "HTTP/1.1 200 OK\r\n\r\n";
    if (getWire == nullptr) {
        return 15;
    }
    ruvia::Http1ClientResponseParser knownLengthParser(getWire->exchangeState());
    const auto knownLengthResult =
        knownLengthParser.parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    const auto* knownLengthHead = knownLengthResult.parsed();
    const auto* knownLength =
        knownLengthHead == nullptr ? nullptr : knownLengthHead->plan().knownLength();
    if (knownLength == nullptr || knownLength->contentLength() != 3 ||
        knownLength->persistence() != ruvia::Http1ClosePolicy::kAllowReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser chunkedParser(getWire->exchangeState());
    const auto chunkedResult =
        chunkedParser.parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    const auto* chunkedHead = chunkedResult.parsed();
    const auto* chunked = chunkedHead == nullptr ? nullptr : chunkedHead->plan().chunked();
    if (chunked == nullptr || chunked->transferCodings().count != 1 ||
        chunked->transferCodings().values[0] != ruvia::HttpTransferCoding::kGzip ||
        chunked->persistence() != ruvia::Http1ClosePolicy::kAllowReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser clientParser(getWire->exchangeState());
    const auto clientResult = clientParser.parse(closeDelimitedHead);
    const auto* clientHead = clientResult.parsed();
    if (clientHead == nullptr || clientHead->plan().closeDelimited() == nullptr ||
        clientHead->consumedBytes() != closeDelimitedHead.size()) {
        return 15;
    }
    ruvia::Http1ClientResponseParser resetContentParser(getWire->exchangeState());
    const auto resetContentResult =
        resetContentParser.parse("HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n");
    const auto* resetContentHead = resetContentResult.parsed();
    const auto* resetContent =
        resetContentHead == nullptr ? nullptr : resetContentHead->plan().zeroContent();
    if (resetContent == nullptr || resetContent->knownLength() == nullptr ||
        resetContent->knownLength()->contentLength() != 0) {
        return 15;
    }

    std::array<char, 256> tunnelHeadBuffer;
    const auto tunnelPrepared =
        ruvia::Http1ClientRequestWriter().prepareConnect(outboundOrigin, {}, tunnelHeadBuffer);
    const auto* tunnelWire = tunnelPrepared.prepared();
    if (tunnelWire == nullptr) {
        return 16;
    }
    ruvia::Http1ClientResponseParser tunnelParser(tunnelWire->exchangeState());
    const auto tunnelResult = tunnelParser.parse(
        "HTTP/1.1 200 Connection Established\r\n"
        "Content-Length: invalid\r\n\r\ntunnel bytes");
    const auto* tunnelHead = tunnelResult.parsed();
    if (tunnelHead == nullptr || tunnelHead->plan().connectTunnel() == nullptr) {
        return 16;
    }

    const ruvia::HttpHeaderView upgradeRequestHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequestView upgradeRequest;
    upgradeRequest.method = "GET";
    upgradeRequest.headers = upgradeRequestHeaders;
    std::array<char, 512> upgradeHeadBuffer;
    const auto upgradePrepared = ruvia::Http1ClientRequestWriter().prepare(
        outboundOrigin, upgradeRequest, upgradeHeadBuffer);
    const auto* upgradeWire = upgradePrepared.prepared();
    if (upgradeWire == nullptr) {
        return 24;
    }
    ruvia::Http1ClientResponseParser upgradeParser(upgradeWire->exchangeState());
    const auto upgradeResult = upgradeParser.parse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket\r\n\r\nopaque bytes");
    const auto* upgradeHead = upgradeResult.parsed();
    if (upgradeHead == nullptr || upgradeHead->plan().protocolUpgrade() == nullptr) {
        return 24;
    }

    const auto streamPlan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"),
        ruvia::Http1ClosePolicy::kAllowReuse);
    if (streamPlan.framing() != ruvia::detail::ResponseStreamFraming::kHttp1Chunked ||
        streamPlan.requestConnectionPlan().disposition() != ruvia::Http1ClosePolicy::kAllowReuse ||
        streamPlan.requestConnectionPlan().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp11 ||
        streamPlan.closePolicy() != ruvia::Http1ClosePolicy::kAllowReuse) {
        return 4;
    }

    ruvia::HttpResponse streamResponse;
    streamResponse.header("Connection", "close");
    const auto preparedStreamResult = ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(streamResponse), ruvia::detail::ResponseStreamKind::kGeneric, streamPlan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedStream = preparedStreamResult.prepared();
    if (preparedStream == nullptr || preparedStreamResult.failure() != nullptr ||
        preparedStream->connectionPlan().disposition() !=
            ruvia::Http1ClosePolicy::kCloseAfterResponse ||
        preparedStream->response().header("Connection") != "close" ||
        preparedStream->responseHeadPlan().chunkedStream() == nullptr ||
        preparedStream->responseHeadPlan().buffered() != nullptr ||
        preparedStream->responseHeadPlan().closeDelimitedStream() != nullptr) {
        return 5;
    }

    const auto http10Plan = ruvia::detail::http1PlanResponseStream(
        serverParser.parseMessage("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
        ruvia::Http1ClosePolicy::kAllowReuse);
    ruvia::HttpResponse resetContentStream;
    resetContentStream.status(ruvia::http_status::kResetContent);
    const auto preparedHttp10Result = ruvia::detail::prepareHttp1ResponseStreamHead(
        std::move(resetContentStream), ruvia::detail::ResponseStreamKind::kGeneric, http10Plan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedHttp10 = preparedHttp10Result.prepared();
    if (preparedHttp10 == nullptr || preparedHttp10Result.failure() != nullptr ||
        !preparedHttp10->commitPlan().bodyPlan().bodySuppressed() ||
        preparedHttp10->commitPlan().headDisposition() !=
            ruvia::detail::ResponseStreamHeadDisposition::kMessageEnded ||
        preparedHttp10->responseHeadPlan().closeDelimitedStream() == nullptr ||
        preparedHttp10->responseHeadPlan().protocolVersion() !=
            ruvia::HttpProtocolVersion::kHttp10 ||
        preparedHttp10->connectionPlan().disposition() != ruvia::Http1ClosePolicy::kAllowReuse ||
        preparedHttp10->response().header("Connection") != "keep-alive") {
        return 13;
    }

    ruvia::HttpResponse response;
    response.body("body");
    const auto writePlan =
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kHead, response);
    if (writePlan.requestMethod() != ruvia::HttpKnownMethod::kHead ||
        writePlan.bodyPlan().requestMethod() != ruvia::HttpKnownMethod::kHead ||
        !writePlan.matchesResponse(response) || !writePlan.bodySuppressed() ||
        writePlan.sendBody() || writePlan.contentLength() != 4) {
        return 6;
    }
    const auto bufferedResponsePlan =
        ruvia::detail::http1BufferedResponsePlan(writePlan, streamPlan.requestConnectionPlan());
    const auto& bufferedHeadPlan = bufferedResponsePlan.headPlan();
    if (bufferedHeadPlan.buffered() == nullptr ||
        bufferedHeadPlan.buffered()->contentLength() != 4 ||
        bufferedResponsePlan.contentLength() != 4 ||
        bufferedResponsePlan.responseStatus() != ruvia::http_status::kOk ||
        bufferedResponsePlan.sendBody() ||
        bufferedHeadPlan.protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 ||
        bufferedHeadPlan.chunkedStream() != nullptr ||
        bufferedHeadPlan.closeDelimitedStream() != nullptr) {
        return 6;
    }

    ruvia::HttpResponse http1ControlResponse;
    http1ControlResponse.header("Connection", "Upgrade");
    http1ControlResponse.header("Upgrade", "websocket");
    const auto http1ControlResult =
        ruvia::detail::http1FinalResponseControlPlan(http1ControlResponse);
    const auto* http1Control = http1ControlResult.control();
    if (http1Control == nullptr || http1ControlResult.failure() != nullptr ||
        !http1Control->connectionOptions().upgrade() ||
        !http1Control->upgradeProtocols().hasProtocol()) {
        return 45;
    }

    const auto http2ControlResult = ruvia::detail::http2FinalResponseControlPlan(response);
    if (http2ControlResult.control() == nullptr || http2ControlResult.failure() != nullptr) {
        return 46;
    }

    ruvia::HttpResponse forbiddenHttp2Control;
    forbiddenHttp2Control.header("Connection", "close");
    const auto forbiddenHttp2ControlResult =
        ruvia::detail::http2FinalResponseControlPlan(forbiddenHttp2Control);
    if (forbiddenHttp2ControlResult.control() != nullptr ||
        forbiddenHttp2ControlResult.failure() == nullptr ||
        forbiddenHttp2ControlResult.failure()->error() !=
            ruvia::detail::Http2FinalResponseControlPlanError::kConnectionSpecificFieldForbidden) {
        return 47;
    }

    const auto h2BufferedHeadResult =
        ruvia::detail::http2BufferedResponseHeadPlan(writePlan, response);
    const auto* h2BufferedHead = h2BufferedHeadResult.plan();
    if (h2BufferedHead == nullptr || h2BufferedHeadResult.failure() != nullptr ||
        h2BufferedHead->contentLength() != std::optional<std::uint64_t>{4} ||
        h2BufferedHead->streamingContentLength().has_value()) {
        return 42;
    }

    ruvia::HttpResponse h2StreamingResponse;
    h2StreamingResponse.header("Content-Length", "0004");
    const auto h2StreamingBodyPlan = ruvia::detail::httpResponseBodyPlan(
        ruvia::HttpKnownMethod::kGet, h2StreamingResponse.status());
    const auto h2StreamingHeadResult =
        ruvia::detail::http2StreamingResponseHeadPlan(h2StreamingBodyPlan, h2StreamingResponse);
    const auto* h2StreamingHead = h2StreamingHeadResult.plan();
    if (h2StreamingHead == nullptr || h2StreamingHeadResult.failure() != nullptr ||
        h2StreamingHead->contentLength() != std::optional<std::uint64_t>{4} ||
        h2StreamingHead->streamingContentLength() != std::optional<std::uint64_t>{4}) {
        return 43;
    }

    h2StreamingResponse.header("Content-Length", "invalid");
    const auto invalidH2StreamingHead =
        ruvia::detail::http2StreamingResponseHeadPlan(h2StreamingBodyPlan, h2StreamingResponse);
    if (invalidH2StreamingHead.plan() != nullptr || invalidH2StreamingHead.failure() == nullptr ||
        invalidH2StreamingHead.failure()->error() !=
            ruvia::detail::Http2ResponseHeadPlanError::kInvalidContentLength) {
        return 44;
    }

    response.status(ruvia::http_status::kResetContent);
    const auto resetContentPlan =
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, response);
    if (resetContentPlan.statusAllowsBody() || !resetContentPlan.bodySuppressed() ||
        resetContentPlan.sendBody() || resetContentPlan.contentLength() != 0) {
        return 9;
    }

    const ruvia::HttpHeaderView earlyHintFields[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(
        ruvia::http_status::kEarlyHints, earlyHintFields);
    if (earlyHints.status() != ruvia::http_status::kEarlyHints ||
        earlyHints.headers().size() != 1) {
        return 26;
    }
    std::array<char, 128> earlyHintsWireBuffer{};
    const auto earlyHintsWire =
        ruvia::Http1InterimResponseWriter().prepare(earlyHints, earlyHintsWireBuffer);
    if (earlyHintsWire.prepared() == nullptr ||
        earlyHintsWire.prepared()->head().find("HTTP/1.1 103 Early Hints\r\n") != 0 ||
        earlyHintsWire.prepared()->connectionDisposition() !=
            ruvia::Http1InterimConnectionDisposition::kUnchanged) {
        return 27;
    }

    const auto installedResolvedRange = ruvia::detail::resolveHttpByteRange("Bytes=10-19", 100);
    const auto installedIgnoredRange = ruvia::detail::resolveHttpByteRange("items=10-19", 100);
    const auto installedUnsatisfiableRange = ruvia::detail::resolveHttpByteRange("bytes=100-", 100);
    if (installedResolvedRange.resolved() == nullptr ||
        installedResolvedRange.resolved()->offset() != 10 ||
        installedResolvedRange.resolved()->length() != 10 ||
        installedResolvedRange.ignored() != nullptr ||
        installedResolvedRange.unsatisfiable() != nullptr ||
        installedIgnoredRange.ignored() == nullptr || installedIgnoredRange.resolved() != nullptr ||
        installedUnsatisfiableRange.unsatisfiable() == nullptr ||
        installedUnsatisfiableRange.resolved() != nullptr) {
        return 33;
    }

    ruvia::detail::Http2PeerSettings installedPeerSettings(ruvia::detail::Http2Role::kServer);
    const auto ordinarySetting =
        installedPeerSettings.apply(ruvia::detail::Http2SettingId::kHeaderTableSize, 8192);
    const auto windowSetting =
        installedPeerSettings.apply(ruvia::detail::Http2SettingId::kInitialWindowSize,
            static_cast<std::uint32_t>(ruvia::detail::kHttp2DefaultInitialWindowSize));
    const auto invalidSetting =
        installedPeerSettings.apply(ruvia::detail::Http2SettingId::kMaxFrameSize, 0);
    if (ordinarySetting.applied() == nullptr || ordinarySetting.initialWindowChange() != nullptr ||
        ordinarySetting.failure() != nullptr || windowSetting.applied() != nullptr ||
        windowSetting.initialWindowChange() == nullptr ||
        windowSetting.initialWindowChange()->delta() != 0 || windowSetting.failure() != nullptr ||
        invalidSetting.applied() != nullptr || invalidSetting.initialWindowChange() != nullptr ||
        invalidSetting.failure() == nullptr ||
        invalidSetting.failure()->error() !=
            ruvia::detail::Http2PeerSettingError::kInvalidMaxFrameSize) {
        return 32;
    }

    ruvia::detail::Http2Connection h2(
        std::pmr::get_default_resource(), ruvia::detail::Http2Role::kClient);
    if (h2.feed({}) != ruvia::detail::Http2FeedResult::kConnectionNotStarted) {
        return 30;
    }
    h2.beginConnection();
    if (h2.connectionError().has_value()) {
        return 12;
    }
    const auto h2WithoutContent = ruvia::detail::Http2RequestContent::none();
    const auto h2ZeroLength = ruvia::detail::Http2RequestContent::knownLength(0);
    const auto h2Streaming = ruvia::detail::Http2RequestContent::streaming();
    if (h2WithoutContent.withoutContent() == nullptr ||
        h2WithoutContent.knownLengthContent() != nullptr ||
        h2ZeroLength.knownLengthContent() == nullptr ||
        h2ZeroLength.knownLengthContent()->length() != 0 ||
        h2Streaming.streamingContent() == nullptr) {
        return 35;
    }
    if (ruvia::detail::isValidOriginFormTarget("*") ||
        !ruvia::detail::isValidOriginOrAsteriskFormTarget(ruvia::HttpKnownMethod::kOptions, "*") ||
        ruvia::detail::isValidOriginOrAsteriskFormTarget(ruvia::HttpKnownMethod::kGet, "*")) {
        return 36;
    }
    if (!ruvia::detail::isValidUriAuthority("deploy:secret@example.test:9418") ||
        ruvia::detail::isValidHostHeader("deploy:secret@example.test:9418")) {
        return 52;
    }
    const auto missingHttpAuthority =
        h2.submitRegularRequestHead("GET", "https", std::nullopt, "/", {}, h2WithoutContent);
    if (missingHttpAuthority.submitted() != nullptr || missingHttpAuthority.failure() == nullptr ||
        missingHttpAuthority.failure()->error() !=
            ruvia::detail::Http2RequestHeadSubmitError::kInvalidMessage) {
        return 51;
    }
    const auto request = h2.submitRegularRequestHead(
        "PROPFIND", "git+ssh", "deploy:secret@example.test:9418", "", {}, h2WithoutContent);
    const auto* submittedRequest = request.submitted();
    if (submittedRequest == nullptr || request.failure() != nullptr) {
        return 7;
    }
    const auto streamId = submittedRequest->streamId();
    const auto* extensionStream = h2.stream(streamId);
    if (extensionStream == nullptr || extensionStream->requestMethod() != "PROPFIND" ||
        extensionStream->requestScheme() != "git+ssh" ||
        extensionStream->requestKnownMethod() != ruvia::HttpKnownMethod::kUnknown ||
        extensionStream->localContent().forbidden() == nullptr ||
        extensionStream->localContent().knownLength() != nullptr ||
        extensionStream->localContent().acceptedBytes() != 0) {
        return 23;
    }
    if (h2.submitData(streamId, "forbidden", ruvia::detail::Http2EndStream::kEndStream) !=
        ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 8;
    }

    const auto serverOptions =
        h2.submitRegularRequestHead("OPTIONS", "https", std::nullopt, "*", {}, h2WithoutContent);
    if (serverOptions.submitted() == nullptr || serverOptions.failure() != nullptr) {
        return 50;
    }

    const auto connect = h2.submitConnectRequestHead("example.test:443");
    const auto* submittedConnect = connect.submitted();
    const auto* connectStream =
        submittedConnect == nullptr ? nullptr : h2.stream(submittedConnect->streamId());
    const auto* pendingConnect =
        connectStream == nullptr ? nullptr : connectStream->tunnel().pending();
    if (submittedConnect == nullptr || connect.failure() != nullptr || pendingConnect == nullptr ||
        connectStream->localSend().connectPending() == nullptr ||
        connectStream->localSend().tunnelOpen() != nullptr ||
        pendingConnect->form() != ruvia::detail::Http2ConnectForm::kStandard ||
        h2.submitData(
            submittedConnect->streamId(), "too early", ruvia::detail::Http2EndStream::kKeepOpen) !=
            ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 10;
    }

    const auto unavailable =
        h2.submitExtendedConnectRequestHead("connect-udp", "https", "example.test", "/masque");
    return unavailable.submitted() == nullptr && unavailable.failure() != nullptr &&
                   unavailable.failure()->error() ==
                       ruvia::detail::Http2RequestHeadSubmitError::kPeerCapabilityUnavailable
               ? 0
               : 11;
}
