template <typename Stream>
Task<void> Http2ServerSession<Stream>::dispatchStream(Http2StreamState& stream) {
    stream.dispatchStarted = true;
    std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
    std::optional<RequestMemory> requestMemoryStorage;
    auto& requestMemory = emplaceRequestMemory(
        requestMemoryStorage,
        memory_,
        std::span<std::byte>(arenaBlock.data(), arenaBlock.size()));

    HttpRequest request;
    if (!Http2RequestBuilder::build(stream, request, remoteAddress_, requestMemory.resource())) {
        HttpResponse response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid http2 request headers"},
            false,
            routeServices());
        co_await writeResponse(stream, response);
        co_return;
    }
    const auto& resolution = stream.routeResolution;
    const auto maxBody = resolution.bodyMode == RequestBodyMode::kStream
        ? options_.maxStreamBodyBytes
        : options_.maxBufferedBodyBytes;
    if (maxBody != 0 && stream.body.size() > maxBody) {
        auto response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 413, .message = "request body is too large"},
            false,
            routeServices());
        co_await writeResponse(stream, response);
        co_return;
    }

    std::optional<Http2RequestBodyReader<Http2ServerSession>> streamReaderStorage;
    std::optional<BodyReader> bodyReaderStorage;
    if (resolution.bodyMode == RequestBodyMode::kStream) {
        streamReaderStorage.emplace(*this, stream.id);
        bodyReaderStorage.emplace(
            &*streamReaderStorage,
            &Http2RequestBodyReader<Http2ServerSession>::readThunk);
    }
    auto* bodyReader = bodyReaderStorage ? &*bodyReaderStorage : nullptr;

    HttpResponse response(requestMemory.resource());
    const bool foundRoute = resolution.found() && resolution.route != nullptr;
    if (foundRoute && resolution.route->responseMode == ResponseBodyMode::kWebSocket) {
        if (co_await dispatchHttp2WebSocketRoute(stream, request, resolution, requestMemory, response)) {
            co_return;
        }
    } else if (foundRoute && resolution.route->responseMode != ResponseBodyMode::kBuffered) {
        if (co_await dispatchHttp2ResponseStreamRoute(
                stream,
                request,
                resolution,
                requestMemory,
                bodyReader,
                response)) {
            co_return;
        }
    } else {
        response = co_await dispatchHttp2BufferedRoute(stream, request, resolution, requestMemory, bodyReader);
    }

    if (stream.reset) {
        co_return;
    }
    const auto responsePreparation = prepareBufferedHttpResponse(
        request,
        response,
        options_,
        &stream.body);
    co_await writeResponse(stream, response, responsePreparation.skipBody);
    if (responsePreparation.borrowedCompressionScratch) {
        stream.body.clear();
    }
}
