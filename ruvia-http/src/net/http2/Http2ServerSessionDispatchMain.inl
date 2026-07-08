template <typename Stream>
Task<void> Http2ServerSession<Stream>::dispatchStream(Http2StreamState& stream) {
    stream.markDispatchStarted();
    const auto requestStart = std::chrono::steady_clock::now();
    std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
    std::optional<RequestMemory> requestMemoryStorage;
    auto& requestMemory = emplaceRequestMemory(
        requestMemoryStorage,
        memory_,
        std::span<std::byte>(arenaBlock.data(), arenaBlock.size()));
    const auto baseServices = routeServices();

    HttpRequest request = HttpRequestAccess::make();
    if (!Http2RequestBuilder::build(stream, request, requestMemory.resource())) {
        HttpResponse response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo(400, {}, "invalid http2 request headers"),
            false,
            baseServices);
        co_await writeResponse(stream, response);
        co_return;
    }
    HttpRequestAccess::setTransport(
        request,
        remoteAddress_,
        clientCertificate_,
        !std::is_same_v<Stream, asio::ip::tcp::socket>);
    const auto& resolution = stream.routeResolution();
    const auto appRateLimit = rateLimitRequestAllowed(rateLimiter_, remoteAddress_);
    if (!appRateLimit.allowed) {
        auto response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo(429, {}, "rate limit exceeded"),
            false,
            baseServices);
        setRetryAfterSeconds(response, std::chrono::milliseconds(appRateLimit.resetAfterMs));
        co_await writeResponse(stream, response);
        recordHttpAccess(
            options_.accessLog, request, remoteAddress_,
            response.status(), requestStart, true);
        co_return;
    }
    const auto maxBody = requestBodyByteLimit(
        stream.bodyMode(),
        options_.maxStreamBodyBytes,
        options_.maxBufferedBodyBytes);
    if (maxBody != 0 && stream.requestBodySize() > maxBody) {
        auto response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo(413, {}, "request body is too large"),
            false,
            baseServices);
        co_await writeResponse(stream, response);
        co_return;
    }

    std::optional<Http2RequestBodyReader<Http2ServerSession>> streamReaderStorage;
    std::optional<BodyReader> bodyReaderStorage;
    if (stream.usesStreamRequestBody()) {
        streamReaderStorage.emplace(*this, stream.id());
        emplaceBodyReaderFacade(bodyReaderStorage, *streamReaderStorage);
    }
    auto dispatchServices = baseServices;
    if (bodyReaderStorage) {
        dispatchServices = dispatchServices.withBodyReader(*bodyReaderStorage);
    }

    HttpResponse response(requestMemory.resource());
    if (resolution.found()) {
        if (resolution.isWebSocketResponse()) {
            auto dispatchResult = co_await dispatchHttp2WebSocketRoute(
                stream,
                request,
                resolution,
                requestMemory,
                baseServices);
            if (dispatchResult.streamHandled()) {
                co_return;
            }
            if (dispatchResult.bufferedResponse()) {
                response = dispatchResult.takeResponse();
            }
        } else if (resolution.usesResponseStream()) {
            auto dispatchResult = co_await dispatchHttp2ResponseStreamRoute(
                stream,
                request,
                resolution,
                requestMemory,
                dispatchServices);
            if (dispatchResult.streamHandled()) {
                // Streamed/committed on the wire; the buffered tail below is skipped,
                // so log the completed streamed response here (status 200).
                recordHttpAccess(
                    options_.accessLog, request, remoteAddress_,
                    response.status(), requestStart, true);
                co_return;
            }
            if (dispatchResult.bufferedResponse()) {
                response = dispatchResult.takeResponse();
            }
        } else {
            response = co_await routes_.dispatchBuffered(
                request,
                resolution,
                requestMemory,
                false,
                dispatchServices);
        }
    } else {
        response = co_await routes_.dispatchBuffered(
            request,
            resolution,
            requestMemory,
            false,
            dispatchServices);
    }

    if (stream.isReset()) {
        co_return;
    }
    const auto responsePreparation = prepareBufferedHttpResponse(
        request,
        response,
        options_,
        stream.responseCompressionScratch());
    co_await writeResponse(stream, response, responsePreparation.skipBody);
    if (responsePreparation.bodyBorrowsCompressionScratch) {
        stream.clearRequestBody();
    }
    recordHttpAccess(
        options_.accessLog, request, remoteAddress_,
        response.status(), requestStart, true);
}
