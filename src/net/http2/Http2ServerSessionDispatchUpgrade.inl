template <typename Stream>
bool Http2ServerSession<Stream>::seedUpgradedStream(
    const HttpParseResult& parsed,
    std::string_view body) {
    auto* stream = createStream(1);
    if (stream == nullptr) {
        return false;
    }
    lastStreamId_ = 1;
    stream->method.assign(methodName(parsed.request.method()));
    stream->scheme.assign("http");
    stream->path.assign(parsed.request.target().data(), parsed.request.target().size());
    const auto host = parsed.request.header(HttpRequest::KnownHeader::kHost);
    if (!host.empty()) {
        stream->authority.assign(host.data(), host.size());
        stream->hasAuthority = true;
        stream->hasHost = true;
    }
    stream->hasMethod = true;
    stream->hasScheme = true;
    stream->hasPath = true;
    if (!parsed.chunked && parsed.contentLength != 0) {
        if (body.size() != parsed.contentLength) {
            return false;
        }
        stream->contentLength = parsed.contentLength;
        stream->hasContentLength = true;
    }
    if (!body.empty()) {
        stream->body.assign(body.data(), body.size());
        stream->receivedBodyBytes = body.size();
    }
    for (const auto& header : parsed.request.headers()) {
        if (http2IsForbiddenUpgradedRequestHeader(header.name)) {
            continue;
        }
        stream->headers.emplace_back(
            header.name,
            header.value,
            classifyRequestHeader(header.name),
            stream->headers.get_allocator().resource());
    }
    stream->headersDecoded = true;
    stream->endStream = true;
    stream->bodyEnded = true;
    resolveStreamRoute(*stream);
    if (stream->bodyMode == RequestBodyMode::kStream && !stream->body.empty()) {
        http2EnqueueOwnedStreamBodyChunk(*stream, stream->body);
    }
    queueReady(stream->id);
    return true;
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeHttp2WebSocketHandshake(
    Http2StreamState& stream,
    std::string_view subprotocol) {
    http2EncodeWebSocketHandshakeHeaders(stream.responseHeaderBlock, subprotocol);
    co_await writeHeaders(stream, stream.responseHeaderBlock, false);
}
