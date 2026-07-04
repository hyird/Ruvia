template <typename Stream>
bool Http2ServerSession<Stream>::seedUpgradedStream(
    const HttpServerParseResult& parsed,
    std::string_view body) {
    auto* stream = createStream(1);
    if (stream == nullptr) {
        return false;
    }
    lastStreamId_ = 1;
    stream->setRequestMethod(parsed.request.method());
    stream->assignRequestPath(parsed.request.target());
    const auto host = requestKnownHeader(parsed.request, RequestKnownHeader::kHost);
    if (!host.empty()) {
        stream->assignRequestAuthority(host);
        stream->markAuthority();
        stream->markHost();
    }
    stream->markMethod();
    stream->markScheme();
    stream->markPath();
    if (!parsed.chunked && parsed.contentLength != 0) {
        if (body.size() != parsed.contentLength) {
            return false;
        }
        if (!stream->setContentLength(parsed.contentLength)) {
            return false;
        }
    }
    if (!body.empty()) {
        stream->assignRequestBody(body);
        stream->setReceivedBodyBytes(body.size());
    }
    for (const auto& header : parsed.request.headers()) {
        if (http2IsForbiddenUpgradedRequestHeader(header.name())) {
            continue;
        }
        if (!stream->appendRequestHeader(
            header.name(),
            header.value(),
            classifyRequestHeader(header.name()))) {
            return false;
        }
    }
    stream->markHeadersDecoded();
    stream->markPeerEndStream();
    stream->markBodyEnded();
    resolveStreamRoute(*stream);
    if (stream->usesStreamRequestBody() && !stream->requestBodyEmpty()) {
        http2EnqueueBufferedRequestBodyChunk(*stream);
    }
    queueReady(stream->id());
    return true;
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeHttp2WebSocketHandshake(
    Http2StreamState& stream,
    std::string_view subprotocol) {
    http2EncodeWebSocketHandshakeHeaders(stream.responseHeaderBlock(), subprotocol);
    co_await writeHeaders(stream, stream.responseHeaderBlock(), false);
    http2ReleaseResponseHeaderBlock(stream);
}
