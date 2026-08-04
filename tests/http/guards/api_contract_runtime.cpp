// Runtime checks that complement the compile-only HTTP API contract guard.
#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
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
#include <ruvia/http/ProtocolByteLimit.h>
#include <ruvia/http/HttpHeader.h>
#include <ruvia/http/HttpClient.h>
#include <ruvia/http/HttpClientRedirect.h>
#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>
#include <ruvia/http/Http1InterimResponseWriter.h>
#include <ruvia/http/Http1RequestParser.h>
#include <ruvia/http/HttpInterimResponse.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/http/HttpProtocolError.h>
#include <ruvia/http/HttpProtocolVersion.h>
#include <ruvia/http/HttpRequest.h>
#include <ruvia/http/HttpResponse.h>
#include <ruvia/http/MultipartParser.h>
#include <ruvia/http/Sse.h>
#include <ruvia/http/UrlEncoding.h>
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
#include <ruvia/http/detail/client/HttpOrigin.h>
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
#include <ruvia/http/detail/parser/MultipartBoundary.h>
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
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketServerHandshake.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeValidation.h>
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
    ruvia::CacheControlFieldParser cacheControlParser;
    cacheControlParser.update("public, max-age=invalid");
    cacheControlParser.update("no-transform, max-age=60");
    const auto installedCacheControl = cacheControlParser.finish();
    if (!installedCacheControl.isPublic || !installedCacheControl.noTransform || installedCacheControl.maxAge.has_value()) {
        return 54;
    }
    if (!ruvia::detail::isValidCookieAttribute("/a path") || ruvia::detail::isValidCookieAttribute("/a\tpath") || ruvia::detail::isValidCookieAttribute("/caf\xc3\xa9") || !ruvia::detail::cookieNameStartsWithIgnoreCase("__sEcUrE-id", "__Secure-")) {
        return 53;
    }
    ruvia::CookieOptions cookieOptions;
    cookieOptions.sameSite = ruvia::CookieSameSite::kLax;
    cookieOptions.maxAge = std::chrono::seconds(60);
    const ruvia::detail::SetCookiePlan cookiePlan("sid", "value", cookieOptions);
    std::array<char, 128> cookieBuffer{};
    cookiePlan.write(cookieBuffer.data());
    if (std::string_view(cookieBuffer.data(), cookiePlan.size()) != "sid=value; Path=/; Max-Age=60; SameSite=Lax") {
        return 52;
    }

    const auto decodedUrl = ruvia::detail::decodeUrlComponent("installed%20decoder", ruvia::detail::UrlDecodeMode::kPercent, std::pmr::get_default_resource());
    const auto malformedUrl = ruvia::detail::decodeUrlComponent("prefix%2", ruvia::detail::UrlDecodeMode::kPercent, std::pmr::get_default_resource());
    if (!decodedUrl.has_value() || std::string_view(*decodedUrl) != "installed decoder" || malformedUrl.has_value()) {
        return 51;
    }
    auto encodedContent = ruvia::detail::encodeHttpContent(ruvia::detail::HttpContentCoding::kGzip, "installed content encoder", 1024, std::pmr::get_default_resource());
    if (encodedContent.encoded() == nullptr || encodedContent.failure() != nullptr || encodedContent.encoded()->bytes().empty()) {
        return 50;
    }
    const auto identityDecode = ruvia::detail::decodeHttpContent(ruvia::detail::HttpContentCoding::kIdentity, "identity", 8, std::pmr::get_default_resource());
    if (identityDecode.decoded() == nullptr || identityDecode.failure() != nullptr || identityDecode.decoded()->bytes() != "identity") {
        return 49;
    }
    const ruvia::HttpResponse emptyResponse;
    const auto& emptyBody = ruvia::detail::responseBody(emptyResponse);
    if (emptyBody.empty() == nullptr || emptyBody.borrowedBytes() != nullptr || emptyBody.staticBytes() != nullptr || emptyBody.ownedBytes() != nullptr || emptyBody.ownedFile() != nullptr || emptyBody.borrowedFile() != nullptr || emptyBody.file().has_value() || !emptyBody.bytes().empty()) {
        return 48;
    }
    std::pmr::monotonic_buffer_resource remoteReceiveResource;
    ruvia::detail::Http2StreamState remoteReceiveStream(3, &remoteReceiveResource);
    const auto& remoteReceive = remoteReceiveStream.remoteReceive();
    if (remoteReceive.headPending() == nullptr || !remoteReceiveStream.beginStandardConnect() || !remoteReceiveStream.finalizeRemoteConnectHead() || remoteReceive.connectPending() == nullptr || !remoteReceiveStream.rejectConnect() || remoteReceive.connectRejectedAwaitingEndStream() == nullptr || !remoteReceiveStream.finishRemoteRejectedConnect() || remoteReceive.endStream() == nullptr) {
        return 39;
    }
    ruvia::detail::Http2StreamState remotePendingEndStream(5, &remoteReceiveResource);
    const auto& pendingEnd = remotePendingEndStream.remoteReceive();
    if (!remotePendingEndStream.beginStandardConnect() || !remotePendingEndStream.finalizeRemoteConnectHead() || !remotePendingEndStream.finishRemotePendingConnect() || pendingEnd.connectPendingEndStream() == nullptr || !remotePendingEndStream.acceptConnect() || pendingEnd.endStream() == nullptr) {
        return 40;
    }

    std::pmr::monotonic_buffer_resource localSendResource;
    ruvia::detail::Http2StreamState localSendStream(1, &localSendResource);
    const auto& localSend = localSendStream.localSend();
    if (localSend.headPending() == nullptr || localSend.responseTrailersOnly() != nullptr || !localSendStream.beginLocalResponseTrailersOnly() || localSend.headPending() != nullptr || localSend.responseTrailersOnly() == nullptr || !localSendStream.queueLocalEndStream() || localSend.endStreamQueued() == nullptr || !localSendStream.commitLocalEndStream() || localSend.endStreamCommitted() == nullptr || !localSendStream.abort(ruvia::detail::Http2StreamCloseSource::kPeer) || localSend.aborted() == nullptr || localSend.aborted()->source() != ruvia::detail::Http2StreamCloseSource::kPeer || localSendStream.abort(static_cast<ruvia::detail::Http2StreamCloseSource>(0xFF))) {
        return 38;
    }

    ruvia::detail::Http2TunnelState tunnel;
    if (tunnel.notConnect() == nullptr || tunnel.pending() != nullptr || !tunnel.begin(ruvia::detail::Http2ConnectForm::kExtended) || tunnel.notConnect() != nullptr || tunnel.pending() == nullptr || tunnel.pending()->form() != ruvia::detail::Http2ConnectForm::kExtended || !tunnel.accept() || tunnel.pending() != nullptr || tunnel.open() == nullptr || tunnel.rejected() != nullptr) {
        return 37;
    }

    ruvia::detail::Http2RemoteContentState remoteContent;
    if (remoteContent.allowedWithoutLength() == nullptr || remoteContent.allowedKnownLength() != nullptr || remoteContent.allowedWithoutLength()->receivedBytes() != 0 || !remoteContent.declareKnownLength(3) || remoteContent.allowedKnownLength() == nullptr || remoteContent.allowedKnownLength()->declaredLength() != 3 || remoteContent.account(2) != ruvia::detail::Http2RemoteContentAccountingResult::kAccepted) {
        return 36;
    }
    if (remoteContent.terminalLengthValid() || remoteContent.account(2) != ruvia::detail::Http2RemoteContentAccountingResult::kDeclaredLengthExceeded || remoteContent.allowedKnownLength()->receivedBytes() != 2) {
        return 36;
    }
    ruvia::detail::Http2RemoteContentState metadataOnly;
    if (!metadataOnly.declareKnownLength(9) || !metadataOnly.selectMetadataOnly() || metadataOnly.metadataOnlyKnownLength() == nullptr || metadataOnly.metadataOnlyKnownLength()->declaredLength() != 9 || metadataOnly.account(1) != ruvia::detail::Http2RemoteContentAccountingResult::kContentForbidden) {
        return 36;
    }

    const auto headSemantics = ruvia::detail::httpResponseContentSemantics(ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
    const auto tunnelSemantics = ruvia::detail::httpResponseContentSemantics(ruvia::HttpKnownMethod::kConnect, ruvia::http_status::kOk);
    if (headSemantics != ruvia::detail::HttpResponseContentSemantics::kWithoutContent || tunnelSemantics != ruvia::detail::HttpResponseContentSemantics::kConnectTunnel) {
        return 36;
    }

    const auto outboundOrigin = ruvia::HttpOrigin::https("example.test");
    ruvia::HttpClientRequest outboundRequest;
    outboundRequest.method = "POST";
    outboundRequest.target = "/submit";
    outboundRequest.content = ruvia::HttpClientRequestContent::bytes("payload");
    const auto* outboundBytes = outboundRequest.content.borrowedBytes();
    if (outboundRequest.content.withoutContent() != nullptr || outboundBytes == nullptr || outboundBytes->value() != "payload") {
        return 34;
    }
    std::array<char, 512> outboundHeadBuffer;
    const auto outboundPrepared = ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, outboundRequest, outboundHeadBuffer);
    const auto* outboundWire = outboundPrepared.prepared();
    const auto* outboundImmediate = outboundWire == nullptr ? nullptr : outboundWire->contentPlan().immediate();
    if (outboundOrigin.scheme() != ruvia::HttpScheme::kHttps || outboundOrigin.host() != "example.test" || outboundOrigin.port() != 443 || outboundRequest.target != "/submit" || outboundWire == nullptr || outboundWire->head().find("Content-Length: 7\r\n") == std::string_view::npos || outboundImmediate == nullptr || outboundImmediate->bytes() != "payload") {
        return 17;
    }
    std::array<char, 512> expectHeadBuffer;
    const auto expectPrepared = ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, outboundRequest, expectHeadBuffer, ruvia::Http1ClientRequestWirePolicy::expectContinue());
    const auto* expectWire = expectPrepared.prepared();
    if (expectWire == nullptr || expectWire->contentPlan().continueGated() == nullptr || expectWire->head().find("Expect: 100-continue\r\n") == std::string_view::npos) {
        return 25;
    }
    ruvia::Http1ClientResponseParser expectParser(*expectWire);
    const auto continueResult = expectParser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    const auto* continueHead = continueResult.parsed();
    if (continueHead == nullptr || continueHead->plan().informational() == nullptr || continueHead->plan().informational()->persistence() != ruvia::Http1ClientResponsePersistence::kReuse || continueHead->head().protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 || continueHead->plan().requestContentSignal() != ruvia::Http1ClientRequestContentSignal::kContinue) {
        return 25;
    }
    if (expectParser.completeRequestContent() != ruvia::Http1ClientRequestContentCompletionStatus::kCompleted) {
        return 25;
    }
    const auto expectFinalResult = expectParser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    const auto* expectFinalHead = expectFinalResult.parsed();
    if (expectFinalHead == nullptr || expectFinalHead->plan().withoutContent() == nullptr || expectFinalHead->plan().requestContentSignal().has_value()) {
        return 25;
    }
    const auto zeroPortOrigin = ruvia::HttpOrigin::http("example.test", 0);
    const auto zeroPortAuthority = ruvia::detail::makeHttpOriginAuthority(zeroPortOrigin, std::pmr::get_default_resource());
    if (zeroPortAuthority != "example.test:0") {
        return 20;
    }
    bool invalidOriginRejected = false;
    try {
        (void)ruvia::HttpOrigin::http("");
    } catch (const std::invalid_argument&) {
        invalidOriginRejected = true;
    }
    if (!invalidOriginRejected) {
        return 21;
    }

    const auto redirectRequestPlan = ruvia::planHttpClientRedirectRequest(outboundRequest, ruvia::http_status::kTemporaryRedirect, std::pmr::get_default_resource());
    if (redirectRequestPlan.method() != "POST" || redirectRequestPlan.contentDisposition() != ruvia::HttpClientRedirectContentDisposition::kPreserve) {
        return 38;
    }

    const auto redirectTarget = ruvia::resolveHttpClientSameOriginRedirectTarget(outboundOrigin, "/base/current", "../next?x=1", std::pmr::get_default_resource());
    if (redirectTarget.target() == nullptr || redirectTarget.failure() != nullptr || redirectTarget.target()->value() != "/next?x=1") {
        return 28;
    }
    const auto crossOrigin = ruvia::resolveHttpClientSameOriginRedirectTarget(outboundOrigin, "/base/current", "https://other.test/next", std::pmr::get_default_resource());
    if (crossOrigin.target() != nullptr || crossOrigin.failure() == nullptr || crossOrigin.failure()->error() != ruvia::HttpClientRedirectTargetError::kNotSameOrigin) {
        return 29;
    }

    const ruvia::HttpProtocolError error(ruvia::http_status::kBadRequest, "bad request");
    if (error.status() != ruvia::http_status::kBadRequest) {
        return 2;
    }
    std::pmr::string wsInput(std::pmr::get_default_resource());
    std::size_t wsOffset = 0;
    std::size_t wsPendingCompactUntil = 0;
    const auto wsNeedInput = ruvia::detail::webSocketTryReadFrame(wsInput, wsOffset, wsPendingCompactUntil, ruvia::ProtocolByteLimit::limited(1024), false);
    if (wsNeedInput.needInput() == nullptr || wsNeedInput.frame() != nullptr || wsNeedInput.failure() != nullptr) {
        return 31;
    }
    const auto multipart = ruvia::parseMultipartBody("--x--\r\n", ruvia::MultipartBoundary("x"));
    if (multipart.failure() != nullptr || multipart.body() == nullptr || !multipart.body()->parts().empty()) {
        return 3;
    }
    ruvia::MultipartParser multipartParser(ruvia::MultipartBoundary("x"), std::pmr::get_default_resource());
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
    ruvia::MultipartParser failedMultipartParser(ruvia::MultipartBoundary("x"), std::pmr::get_default_resource());
    failedMultipartParser.feed(std::string(64 * 1024 + 1, 'p'));
    const auto multipartFailure = failedMultipartParser.poll();
    const auto repeatedMultipartFailure = failedMultipartParser.poll();
    if (multipartFailure.failure() == nullptr || repeatedMultipartFailure.failure() == nullptr || multipartFailure.failure()->protocolError().status() != ruvia::http_status::kContentTooLarge || std::string_view(repeatedMultipartFailure.failure()->protocolError().what()) != multipartFailure.failure()->protocolError().what()) {
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
    if (parsed == nullptr || parsed->bodyPlan().chunked() == nullptr || parsed->request().protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 || parsed->wireBody() != "1\r\nx\r\n0\r\n\r\n" || parsed->consumedBytes() != chunkedRequest.size()) {
        return 1;
    }

    ruvia::detail::Http1ServerRequestParser serverParser;
    const auto extensionMethod = serverParser.parseMessage("PROPFIND /dav HTTP/1.1\r\nHost: example.test\r\n\r\n");
    if (!extensionMethod.messageReady() || extensionMethod.request.method() != "PROPFIND" || extensionMethod.request.knownMethod() != ruvia::HttpKnownMethod::kUnknown) {
        return 22;
    }
    const auto transferCoded = serverParser.parseMessage(
        "POST / HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n"
        "1\r\nx\r\n0\r\n\r\n");
    const auto* transferCodedBody = transferCoded.bodyPlan.chunked();
    if (!transferCoded.messageReady() || transferCodedBody == nullptr || transferCodedBody->transferCodings().count != 1 || transferCodedBody->transferCodings().values[0] != ruvia::detail::HttpTransferCoding::kGzip) {
        return 14;
    }

    ruvia::HttpClientRequest getRequest;
    getRequest.method = "GET";
    std::array<char, 256> getHeadBuffer;
    const auto getPrepared = ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, getRequest, getHeadBuffer);
    const auto* getWire = getPrepared.prepared();
    constexpr std::string_view closeDelimitedHead = "HTTP/1.1 200 OK\r\n\r\n";
    if (getWire == nullptr) {
        return 15;
    }
    ruvia::Http1ClientResponseParser knownLengthParser(*getWire);
    const auto knownLengthResult = knownLengthParser.parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    const auto* knownLengthHead = knownLengthResult.parsed();
    const auto* knownLength = knownLengthHead == nullptr ? nullptr : knownLengthHead->plan().knownLength();
    if (knownLength == nullptr || knownLength->contentLength() != 3 || knownLength->persistence() != ruvia::Http1ClientResponsePersistence::kReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser chunkedParser(*getWire);
    const auto chunkedResult = chunkedParser.parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n");
    const auto* chunkedHead = chunkedResult.parsed();
    const auto* chunked = chunkedHead == nullptr ? nullptr : chunkedHead->plan().chunked();
    if (chunked == nullptr || chunked->transferCodings().count != 1 || chunked->transferCodings().values[0] != ruvia::detail::HttpTransferCoding::kGzip || chunked->persistence() != ruvia::Http1ClientResponsePersistence::kReuse) {
        return 15;
    }
    ruvia::Http1ClientResponseParser clientParser(*getWire);
    const auto clientResult = clientParser.parse(closeDelimitedHead);
    const auto* clientHead = clientResult.parsed();
    if (clientHead == nullptr || clientHead->plan().closeDelimited() == nullptr || clientHead->consumedBytes() != closeDelimitedHead.size()) {
        return 15;
    }
    ruvia::Http1ClientResponseParser resetContentParser(*getWire);
    const auto resetContentResult = resetContentParser.parse("HTTP/1.1 205 Reset Content\r\nContent-Length: 0\r\n\r\n");
    const auto* resetContentHead = resetContentResult.parsed();
    const auto* resetContent = resetContentHead == nullptr ? nullptr : resetContentHead->plan().zeroContent();
    if (resetContent == nullptr || resetContent->knownLength() == nullptr || resetContent->knownLength()->contentLength() != 0) {
        return 15;
    }

    std::array<char, 256> tunnelHeadBuffer;
    const auto tunnelPrepared = ruvia::Http1ClientRequestWriter().prepareConnect(outboundOrigin, {}, tunnelHeadBuffer);
    const auto* tunnelWire = tunnelPrepared.prepared();
    if (tunnelWire == nullptr) {
        return 16;
    }
    ruvia::Http1ClientResponseParser tunnelParser(*tunnelWire);
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
    ruvia::HttpClientRequest upgradeRequest;
    upgradeRequest.method = "GET";
    upgradeRequest.headers = upgradeRequestHeaders;
    std::array<char, 512> upgradeHeadBuffer;
    const auto upgradePrepared = ruvia::Http1ClientRequestWriter().prepare(outboundOrigin, upgradeRequest, upgradeHeadBuffer);
    const auto* upgradeWire = upgradePrepared.prepared();
    if (upgradeWire == nullptr) {
        return 24;
    }
    ruvia::Http1ClientResponseParser upgradeParser(*upgradeWire);
    const auto upgradeResult = upgradeParser.parse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket\r\n\r\nopaque bytes");
    const auto* upgradeHead = upgradeResult.parsed();
    if (upgradeHead == nullptr || upgradeHead->plan().protocolUpgrade() == nullptr) {
        return 24;
    }

    const auto streamPlan = ruvia::detail::http1PlanResponseStream(serverParser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n"), ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    if (streamPlan.framing() != ruvia::detail::ResponseStreamFraming::kHttp1Chunked || streamPlan.requestConnectionPlan().disposition() != ruvia::detail::Http1ConnectionDisposition::kReuse || streamPlan.requestConnectionPlan().protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 || streamPlan.closePolicy() != ruvia::detail::Http1ServerClosePolicy::kAllowReuse) {
        return 4;
    }

    ruvia::HttpResponse streamResponse;
    streamResponse.header("Connection", "close");
    const auto preparedStreamResult = ruvia::detail::prepareHttp1ResponseStreamHead(std::move(streamResponse), ruvia::detail::ResponseStreamKind::kGeneric, streamPlan, ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedStream = preparedStreamResult.prepared();
    if (preparedStream == nullptr || preparedStreamResult.failure() != nullptr || preparedStream->connectionPlan().disposition() != ruvia::detail::Http1ConnectionDisposition::kClose || preparedStream->response().header("Connection") != "close" || preparedStream->responseHeadPlan().chunkedStream() == nullptr || preparedStream->responseHeadPlan().buffered() != nullptr || preparedStream->responseHeadPlan().closeDelimitedStream() != nullptr) {
        return 5;
    }

    const auto http10Plan = ruvia::detail::http1PlanResponseStream(serverParser.parseMessage("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"), ruvia::detail::Http1ServerClosePolicy::kAllowReuse);
    ruvia::HttpResponse resetContentStream;
    resetContentStream.status(ruvia::http_status::kResetContent);
    const auto preparedHttp10Result = ruvia::detail::prepareHttp1ResponseStreamHead(std::move(resetContentStream), ruvia::detail::ResponseStreamKind::kGeneric, http10Plan, ruvia::detail::ResponseTrailerIntent::kNone);
    const auto* preparedHttp10 = preparedHttp10Result.prepared();
    if (preparedHttp10 == nullptr || preparedHttp10Result.failure() != nullptr || !preparedHttp10->commitPlan().bodyPlan().bodySuppressed() || preparedHttp10->commitPlan().headDisposition() != ruvia::detail::ResponseStreamHeadDisposition::kMessageEnded || preparedHttp10->responseHeadPlan().closeDelimitedStream() == nullptr || preparedHttp10->responseHeadPlan().protocolVersion() != ruvia::HttpProtocolVersion::kHttp10 || preparedHttp10->connectionPlan().disposition() != ruvia::detail::Http1ConnectionDisposition::kReuse || preparedHttp10->response().header("Connection") != "keep-alive") {
        return 13;
    }

    ruvia::HttpResponse response;
    response.body("body");
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kHead, response);
    if (writePlan.requestMethod() != ruvia::HttpKnownMethod::kHead || writePlan.bodyPlan().requestMethod() != ruvia::HttpKnownMethod::kHead || !writePlan.matchesResponse(response) || !writePlan.bodySuppressed() || writePlan.sendBody() || writePlan.contentLength() != 4) {
        return 6;
    }
    const auto bufferedResponsePlan = ruvia::detail::http1BufferedResponsePlan(writePlan, streamPlan.requestConnectionPlan());
    const auto& bufferedHeadPlan = bufferedResponsePlan.headPlan();
    if (bufferedHeadPlan.buffered() == nullptr || bufferedHeadPlan.buffered()->contentLength() != 4 || bufferedResponsePlan.contentLength() != 4 || bufferedResponsePlan.responseStatus() != ruvia::http_status::kOk || bufferedResponsePlan.sendBody() || bufferedHeadPlan.protocolVersion() != ruvia::HttpProtocolVersion::kHttp11 || bufferedHeadPlan.chunkedStream() != nullptr || bufferedHeadPlan.closeDelimitedStream() != nullptr) {
        return 6;
    }

    ruvia::HttpResponse http1ControlResponse;
    http1ControlResponse.header("Connection", "Upgrade");
    http1ControlResponse.header("Upgrade", "websocket");
    const auto http1ControlResult = ruvia::detail::http1FinalResponseControlPlan(http1ControlResponse);
    const auto* http1Control = http1ControlResult.control();
    if (http1Control == nullptr || http1ControlResult.failure() != nullptr || !http1Control->connectionOptions().upgrade() || !http1Control->upgradeProtocols().hasProtocol()) {
        return 45;
    }

    const auto http2ControlResult = ruvia::detail::http2FinalResponseControlPlan(response);
    if (http2ControlResult.control() == nullptr || http2ControlResult.failure() != nullptr) {
        return 46;
    }

    ruvia::HttpResponse forbiddenHttp2Control;
    forbiddenHttp2Control.header("Connection", "close");
    const auto forbiddenHttp2ControlResult = ruvia::detail::http2FinalResponseControlPlan(forbiddenHttp2Control);
    if (forbiddenHttp2ControlResult.control() != nullptr || forbiddenHttp2ControlResult.failure() == nullptr || forbiddenHttp2ControlResult.failure()->error() != ruvia::detail::Http2FinalResponseControlPlanError::kConnectionSpecificFieldForbidden) {
        return 47;
    }

    const auto h2BufferedHeadResult = ruvia::detail::http2BufferedResponseHeadPlan(writePlan, response);
    const auto* h2BufferedHead = h2BufferedHeadResult.plan();
    if (h2BufferedHead == nullptr || h2BufferedHeadResult.failure() != nullptr || h2BufferedHead->contentLength() != std::optional<std::uint64_t>{4} || h2BufferedHead->streamingContentLength().has_value()) {
        return 42;
    }

    ruvia::HttpResponse h2StreamingResponse;
    h2StreamingResponse.header("Content-Length", "0004");
    const auto h2StreamingBodyPlan = ruvia::detail::httpResponseBodyPlan(ruvia::HttpKnownMethod::kGet, h2StreamingResponse.status());
    const auto h2StreamingHeadResult = ruvia::detail::http2StreamingResponseHeadPlan(h2StreamingBodyPlan, h2StreamingResponse);
    const auto* h2StreamingHead = h2StreamingHeadResult.plan();
    if (h2StreamingHead == nullptr || h2StreamingHeadResult.failure() != nullptr || h2StreamingHead->contentLength() != std::optional<std::uint64_t>{4} || h2StreamingHead->streamingContentLength() != std::optional<std::uint64_t>{4}) {
        return 43;
    }

    h2StreamingResponse.header("Content-Length", "invalid");
    const auto invalidH2StreamingHead = ruvia::detail::http2StreamingResponseHeadPlan(h2StreamingBodyPlan, h2StreamingResponse);
    if (invalidH2StreamingHead.plan() != nullptr || invalidH2StreamingHead.failure() == nullptr || invalidH2StreamingHead.failure()->error() != ruvia::detail::Http2ResponseHeadPlanError::kInvalidContentLength) {
        return 44;
    }

    response.status(ruvia::http_status::kResetContent);
    const auto resetContentPlan = ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, response);
    if (resetContentPlan.statusAllowsBody() || !resetContentPlan.bodySuppressed() || resetContentPlan.sendBody() || resetContentPlan.contentLength() != 0) {
        return 9;
    }

    const ruvia::HttpHeaderView earlyHintFields[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(ruvia::http_status::kEarlyHints, earlyHintFields);
    if (earlyHints.status() != ruvia::http_status::kEarlyHints || earlyHints.headers().size() != 1) {
        return 26;
    }
    std::array<char, 128> earlyHintsWireBuffer{};
    const auto earlyHintsWire = ruvia::Http1InterimResponseWriter().prepare(earlyHints, earlyHintsWireBuffer);
    if (earlyHintsWire.prepared() == nullptr || earlyHintsWire.prepared()->head().find("HTTP/1.1 103 Early Hints\r\n") != 0 || earlyHintsWire.prepared()->connectionDisposition() != ruvia::Http1InterimConnectionDisposition::kUnchanged) {
        return 27;
    }

    const auto installedResolvedRange = ruvia::detail::resolveHttpByteRange("Bytes=10-19", 100);
    const auto installedIgnoredRange = ruvia::detail::resolveHttpByteRange("items=10-19", 100);
    const auto installedUnsatisfiableRange = ruvia::detail::resolveHttpByteRange("bytes=100-", 100);
    if (installedResolvedRange.resolved() == nullptr || installedResolvedRange.resolved()->offset() != 10 || installedResolvedRange.resolved()->length() != 10 || installedResolvedRange.ignored() != nullptr || installedResolvedRange.unsatisfiable() != nullptr || installedIgnoredRange.ignored() == nullptr || installedIgnoredRange.resolved() != nullptr || installedUnsatisfiableRange.unsatisfiable() == nullptr || installedUnsatisfiableRange.resolved() != nullptr) {
        return 33;
    }

    ruvia::detail::Http2PeerSettings installedPeerSettings(ruvia::detail::Http2Role::kServer);
    const auto ordinarySetting = installedPeerSettings.apply(ruvia::detail::Http2SettingId::kHeaderTableSize, 8192);
    const auto windowSetting = installedPeerSettings.apply(ruvia::detail::Http2SettingId::kInitialWindowSize, static_cast<std::uint32_t>(ruvia::detail::kHttp2DefaultInitialWindowSize));
    const auto invalidSetting = installedPeerSettings.apply(ruvia::detail::Http2SettingId::kMaxFrameSize, 0);
    if (ordinarySetting.applied() == nullptr || ordinarySetting.initialWindowChange() != nullptr || ordinarySetting.failure() != nullptr || windowSetting.applied() != nullptr || windowSetting.initialWindowChange() == nullptr || windowSetting.initialWindowChange()->delta() != 0 || windowSetting.failure() != nullptr || invalidSetting.applied() != nullptr || invalidSetting.initialWindowChange() != nullptr || invalidSetting.failure() == nullptr || invalidSetting.failure()->error() != ruvia::detail::Http2PeerSettingError::kInvalidMaxFrameSize) {
        return 32;
    }

    ruvia::detail::Http2Connection h2(std::pmr::get_default_resource(), ruvia::detail::Http2Role::kClient);
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
    if (h2WithoutContent.withoutContent() == nullptr || h2WithoutContent.knownLengthContent() != nullptr || h2ZeroLength.knownLengthContent() == nullptr || h2ZeroLength.knownLengthContent()->length() != 0 || h2Streaming.streamingContent() == nullptr) {
        return 35;
    }
    if (ruvia::detail::isValidOriginFormTarget("*") || !ruvia::detail::isValidOriginOrAsteriskFormTarget(ruvia::HttpKnownMethod::kOptions, "*") || ruvia::detail::isValidOriginOrAsteriskFormTarget(ruvia::HttpKnownMethod::kGet, "*")) {
        return 36;
    }
    if (!ruvia::detail::isValidUriAuthority("deploy:secret@example.test:9418") || ruvia::detail::isValidHostHeader("deploy:secret@example.test:9418")) {
        return 52;
    }
    const auto missingHttpAuthority = h2.submitRegularRequestHead("GET", "https", std::nullopt, "/", {}, h2WithoutContent);
    if (missingHttpAuthority.submitted() != nullptr || missingHttpAuthority.failure() == nullptr || missingHttpAuthority.failure()->error() != ruvia::detail::Http2RequestHeadSubmitError::kInvalidMessage) {
        return 51;
    }
    const auto request = h2.submitRegularRequestHead("PROPFIND", "git+ssh", "deploy:secret@example.test:9418", "", {}, h2WithoutContent);
    const auto* submittedRequest = request.submitted();
    if (submittedRequest == nullptr || request.failure() != nullptr) {
        return 7;
    }
    const auto streamId = submittedRequest->streamId();
    const auto* extensionStream = h2.stream(streamId);
    if (extensionStream == nullptr || extensionStream->requestMethod() != "PROPFIND" || extensionStream->requestScheme() != "git+ssh" || extensionStream->requestKnownMethod() != ruvia::HttpKnownMethod::kUnknown || extensionStream->localContent().forbidden() == nullptr || extensionStream->localContent().knownLength() != nullptr || extensionStream->localContent().acceptedBytes() != 0) {
        return 23;
    }
    if (h2.submitData(streamId, "forbidden", ruvia::detail::Http2EndStream::kEndStream) != ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 8;
    }

    const auto serverOptions = h2.submitRegularRequestHead("OPTIONS", "https", std::nullopt, "*", {}, h2WithoutContent);
    if (serverOptions.submitted() == nullptr || serverOptions.failure() != nullptr) {
        return 50;
    }

    const auto connect = h2.submitConnectRequestHead("example.test:443");
    const auto* submittedConnect = connect.submitted();
    const auto* connectStream = submittedConnect == nullptr ? nullptr : h2.stream(submittedConnect->streamId());
    const auto* pendingConnect = connectStream == nullptr ? nullptr : connectStream->tunnel().pending();
    if (submittedConnect == nullptr || connect.failure() != nullptr || pendingConnect == nullptr || connectStream->localSend().connectPending() == nullptr || connectStream->localSend().tunnelOpen() != nullptr || pendingConnect->form() != ruvia::detail::Http2ConnectForm::kStandard || h2.submitData(submittedConnect->streamId(), "too early", ruvia::detail::Http2EndStream::kKeepOpen) != ruvia::detail::Http2DataSubmitStatus::kInvalidState) {
        return 10;
    }

    const auto unavailable = h2.submitExtendedConnectRequestHead("connect-udp", "https", "example.test", "/masque");
    return unavailable.submitted() == nullptr && unavailable.failure() != nullptr && unavailable.failure()->error() == ruvia::detail::Http2RequestHeadSubmitError::kPeerCapabilityUnavailable ? 0 : 11;
}
