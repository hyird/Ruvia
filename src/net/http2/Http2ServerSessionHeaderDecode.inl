template <typename Stream>
Task<bool> Http2ServerSession<Stream>::handleHeaderDecodeFailure(
    Http2StreamState& stream,
    HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kCompressionError, "invalid HPACK block");
        co_return false;
    }
    co_await sendRstStream(stream.id, Http2ErrorCode::kProtocolError);
    stream.reset = true;
    stream.closeSource = Http2StreamCloseSource::kLocal;
    co_return true;
}

template <typename Stream>
HeaderDecodeStatus Http2ServerSession<Stream>::decodeHeaderBlock(Http2StreamState& stream) {
    const auto result = decoder_.decode(stream.headerBlock, &stream, [](void* target, std::string_view name, std::string_view value) {
        return http2OnDecodedInitialHeader(*static_cast<Http2StreamState*>(target), name, value);
    });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.hasMethod) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.hasProtocol) {
        if (stream.method != "CONNECT" ||
            stream.protocol != "websocket" ||
            !stream.hasScheme ||
            !stream.hasPath ||
            !stream.hasAuthority) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.extendedConnectWebSocket = true;
    } else if (stream.method == "CONNECT") {
        if (!stream.hasAuthority || stream.hasScheme || stream.hasPath) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.standardConnect = true;
    } else if (!stream.hasScheme || !stream.hasPath) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.hasContentLength && stream.body.size() > stream.contentLength) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.headersDecoded = true;
    resolveStreamRoute(stream);
    return HeaderDecodeStatus::kOk;
}

template <typename Stream>
HeaderDecodeStatus Http2ServerSession<Stream>::finishTrailerBlock(Http2StreamState& stream) {
    const auto result = decoder_.decode(stream.headerBlock, &stream, [](void* target, std::string_view name, std::string_view value) {
        return http2OnDecodedTrailer(*static_cast<Http2StreamState*>(target), name, value);
    });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (stream.hasContentLength && stream.receivedBodyBytes != stream.contentLength) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.endStream = true;
    stream.bodyEnded = true;
    if (stream.bodyMode == RequestBodyMode::kStream) {
        if (!stream.dispatchStarted) {
            queueReady(stream.id);
        }
        resumeBodyWaiter(stream);
    } else {
        queueReady(stream.id);
    }
    return HeaderDecodeStatus::kOk;
}

template <typename Stream>
void Http2ServerSession<Stream>::queueInitialStreamIfReady(Http2StreamState& stream) {
    if (stream.endStream || stream.standardConnect) {
        stream.bodyEnded = true;
        queueReady(stream.id);
    } else if (stream.bodyMode == RequestBodyMode::kStream) {
        queueReady(stream.id);
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::resolveStreamRoute(Http2StreamState& stream) noexcept {
    HttpRequest request;
    if (!Http2RequestBuilder::build(stream, request, remoteAddress_, memory_.resource())) {
        stream.routeResolution = {};
        stream.bodyMode = RequestBodyMode::kBuffered;
        return;
    }
    stream.routeResolution = routes_.resolve(request);
    stream.bodyMode = stream.routeResolution.bodyMode;
    if (stream.extendedConnectWebSocket &&
        stream.routeResolution.found() &&
        stream.routeResolution.route != nullptr &&
        stream.routeResolution.route->responseMode == ResponseBodyMode::kWebSocket) {
        stream.webSocketTunnel = true;
        stream.bodyMode = RequestBodyMode::kStream;
    }
}
