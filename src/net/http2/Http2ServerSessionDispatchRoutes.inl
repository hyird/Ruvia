template <typename Stream>
RouteServices Http2ServerSession<Stream>::routeServices(BodyReader* bodyReader) const noexcept {
    return RouteServices{
        .db = databases_,
        .redis = redis_,
        .bodyReader = bodyReader,
        .httpClients = httpClients_};
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::dispatchHttp2WebSocketRoute(
    Http2StreamState& stream,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    HttpResponse& response) {
    if (!http2IsValidWebSocketRequest(stream, request)) {
        response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid http2 websocket request"},
            false,
            routeServices());
        co_return false;
    }

    co_await writeHttp2WebSocketHandshake(
        stream,
        http2ChooseWebSocketSubprotocol(request, resolution.route->webSocketSubprotocols));
    Http2WebSocketConnection<Http2ServerSession> webSocketConnection(
        Http2WebSocketTransport<Http2ServerSession>{*this, stream},
        scannerEntry_,
        resolution.route->webSocketHeartbeat,
        options_.maxWebSocketMessageBytes,
        memory_.resource());
    co_await runWebSocketSession(
        webSocketConnection,
        scannerEntry_,
        routes_,
        request,
        resolution,
        requestMemory,
        routeServices());
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::dispatchHttp2ResponseStreamRoute(
    Http2StreamState& stream,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    BodyReader* bodyReader,
    HttpResponse& response) {
    Http2ResponseStreamSink<Http2ServerSession> responseSink(*this, stream, resolution.route->responseMode);
    ResponseStreamWriter responseStream(
        &responseSink,
        &Http2ResponseStreamSink<Http2ServerSession>::writeThunk,
        &Http2ResponseStreamSink<Http2ServerSession>::endThunk,
        &Http2ResponseStreamSink<Http2ServerSession>::bindContextThunk,
        &Http2ResponseStreamSink<Http2ServerSession>::scratchThunk);
    std::exception_ptr exception;
    bool streamHandled = false;
    try {
        auto result = co_await routes_.dispatchResponseStream(
            request,
            resolution,
            requestMemory,
            responseStream,
            routeServices(bodyReader));
        streamHandled = result.streamHandled;
        if (stream.reset) {
            co_return true;
        }
        if (streamHandled || responseSink.committed()) {
            co_await responseStream.end();
            co_return true;
        }
        response = std::move(result.response);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        if (responseSink.committed()) {
            co_await sendRstStream(stream.id, Http2ErrorCode::kInternalError);
            stream.reset = true;
            co_return true;
        }
        response = co_await routes_.handleException(
            request,
            requestMemory,
            exception,
            false,
            routeServices(bodyReader));
    }
    co_return false;
}

template <typename Stream>
Task<HttpResponse> Http2ServerSession<Stream>::dispatchHttp2BufferedRoute(
    Http2StreamState&,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    BodyReader* bodyReader) {
    HttpResponse response(requestMemory.resource());
    std::exception_ptr exception;
    try {
        response = co_await routes_.dispatch(
            request,
            resolution,
            requestMemory,
            routeServices(bodyReader));
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception != nullptr) {
        response = co_await routes_.handleException(
            request,
            requestMemory,
            exception,
            false,
            routeServices(bodyReader));
    }
    co_return response;
}
