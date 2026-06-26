template <typename Stream>
ContextServices Http2ServerSession<Stream>::routeServices() const noexcept {
    return ContextServices(databases_, redis_, httpClients_);
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::dispatchHttp2WebSocketRoute(
    Http2StreamState& stream,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    ContextServices services,
    HttpResponse& response) {
    if (!http2IsValidWebSocketRequest(stream, request)) {
        response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid http2 websocket request"},
            false,
            services);
        co_return false;
    }

    const auto& route = resolution.route();
    co_await writeHttp2WebSocketHandshake(
        stream,
        http2ChooseWebSocketSubprotocol(request, route.webSocketSubprotocols()));
    Http2WebSocketConnection<Http2ServerSession> webSocketConnection(
        Http2WebSocketTransport<Http2ServerSession>{*this, stream},
        scannerEntry_,
        route.webSocketHeartbeat(),
        options_.maxWebSocketMessageBytes,
        memory_.resource());
    co_await runWebSocketSession(
        webSocketConnection,
        scannerEntry_,
        routes_,
        request,
        resolution,
        requestMemory,
        services);
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::dispatchHttp2ResponseStreamRoute(
    Http2StreamState& stream,
    const HttpRequest& request,
    const RouteResolution& resolution,
    RequestMemory& requestMemory,
    ContextServices services,
    HttpResponse& response) {
    const auto& route = resolution.route();
    Http2ResponseStreamSink<Http2ServerSession> responseSink(*this, stream, route.responseMode());
    auto result = co_await dispatchResponseStreamWith(
        responseSink,
        routes_,
        request,
        resolution,
        requestMemory,
        services,
        /*closeConnectionOnError=*/false,
        /*peerAborted=*/[&stream]() noexcept { return stream.isReset(); });

    if (result.streamed() || result.abortedByPeer()) {
        co_return true;
    }
    if (result.abortedAfterCommit()) {
        co_await sendRstStream(stream.id(), Http2ErrorCode::kInternalError);
        stream.markReset();
        co_return true;
    }
    if (result.hasBufferedResponse()) {
        response = result.takeResponse();
        co_return false;
    }
    co_return false;
}
