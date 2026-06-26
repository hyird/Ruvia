template <typename Stream>
Task<bool> Http2ServerSession<Stream>::handleHeaderDecodeFailure(
    Http2StreamState& stream,
    HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kCompressionError, "invalid HPACK block");
        co_return false;
    }
    co_await sendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    stream.markReset();
    co_return true;
}

template <typename Stream>
HeaderDecodeStatus Http2ServerSession<Stream>::decodeHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(),
        &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target),
                name,
                value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.hasMethod()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.hasProtocol()) {
        if (stream.requestMethod() != HttpMethod::kConnect ||
            !stream.protocolIsWebSocket() ||
            !stream.hasScheme() ||
            !stream.hasPath() ||
            !stream.hasAuthority()) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.markExtendedConnectWebSocket();
    } else if (stream.requestMethod() == HttpMethod::kConnect) {
        if (!stream.hasAuthority() ||
            stream.hasScheme() ||
            stream.hasPath()) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.markStandardConnect();
    } else if (!stream.hasScheme() || !stream.hasPath()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.bufferedBodyExceedsContentLength()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markHeadersDecoded();
    resolveStreamRoute(stream);
    return HeaderDecodeStatus::kOk;
}

template <typename Stream>
HeaderDecodeStatus Http2ServerSession<Stream>::finishTrailerBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(),
        &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedTrailer(
                *static_cast<Http2HeaderDecodeContext*>(target),
                name,
                value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.bodyLengthComplete()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markPeerEndStream();
    stream.markBodyEnded();
    if (stream.usesStreamRequestBody()) {
        if (!stream.dispatchStarted()) {
            queueReady(stream.id());
        }
        resumeBodyWaiter(stream);
    } else {
        queueReady(stream.id());
    }
    return HeaderDecodeStatus::kOk;
}

template <typename Stream>
HeaderDecodeStatus Http2ServerSession<Stream>::decodeRefusedHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(),
        &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target),
                name,
                value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    return HeaderDecodeStatus::kOk;
}

template <typename Stream>
void Http2ServerSession<Stream>::queueInitialStreamIfReady(Http2StreamState& stream) {
    if (stream.peerEndStream() || stream.standardConnect()) {
        stream.markBodyEnded();
        queueReady(stream.id());
    } else if (stream.usesStreamRequestBody()) {
        queueReady(stream.id());
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::resolveStreamRoute(Http2StreamState& stream) noexcept {
    const auto method = Http2RequestBuilder::requestMethod(stream);
    const auto path = Http2RequestBuilder::requestPath(stream);
    if (method == HttpMethod::kUnknown || path.empty()) {
        stream.resetRoutingToBuffered();
        return;
    }
    auto& match = stream.routeMatch();
    stream.setRouteResolution(routes_.resolve(method, path, match));
    const auto& resolution = stream.routeResolution();
    if (!resolution.found()) {
        stream.setBodyMode(RequestBodyMode::kBuffered);
        return;
    }

    const auto& route = resolution.route();
    stream.setBodyMode(route.bodyMode());
    if (stream.extendedConnectWebSocket() && route.isWebSocketResponse()) {
        stream.markWebSocketTunnel();
        stream.setBodyMode(RequestBodyMode::kStream);
    }
}
