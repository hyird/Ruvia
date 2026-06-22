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
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes_,
        request,
        resolution,
        requestMemory,
        routeServices(bodyReader),
        /*closeConnectionOnError=*/false,
        /*peerAborted=*/[&stream]() noexcept { return stream.reset; });

    switch (result.outcome) {
        case ResponseStreamDispatchOutcome::kStreamed:
        case ResponseStreamDispatchOutcome::kAbortedByPeer:
            co_return true;
        case ResponseStreamDispatchOutcome::kAbortedAfterCommit:
            co_await sendRstStream(stream.id, Http2ErrorCode::kInternalError);
            stream.reset = true;
            co_return true;
        case ResponseStreamDispatchOutcome::kBuffered:
        case ResponseStreamDispatchOutcome::kFailedBeforeCommit:
            response = std::move(result.response);
            co_return false;
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
    co_return co_await routes_.dispatchBuffered(
        request, resolution, requestMemory, false, routeServices(bodyReader));
}
