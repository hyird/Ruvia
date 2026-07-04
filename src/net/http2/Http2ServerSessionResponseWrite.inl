template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeHeaders(
    Http2StreamState& stream,
    std::string_view headerBlock,
    bool endStream) {
    if (stream.isReset()) {
        co_return;
    }
    std::size_t offset = 0;
    bool first = true;
    while (offset < headerBlock.size() || first) {
        const auto remaining = headerBlock.size() - offset;
        const auto chunk = static_cast<std::uint32_t>(
            std::min<std::size_t>(remaining, peerSettings_.maxFrameSize()));
        const bool last = offset + chunk == headerBlock.size();
        const auto flags = static_cast<std::uint8_t>(
            (last ? kHttp2FlagEndHeaders : 0) |
            (endStream && last ? kHttp2FlagEndStream : 0));
        co_await writeFramePayload(
            first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation,
            flags,
            stream.id(),
            headerBlock.substr(offset, chunk));
        offset += chunk;
        first = false;
    }
}

template <typename Stream>
Task<Http2DataWindowResult> Http2ServerSession<Stream>::waitForDataWindow(Http2StreamState& stream) {
    while (!closing_ && !http2SendWindowAvailable(connectionSendWindow_, stream)) {
        if (readerRunning_) {
            co_await Http2SendWindowAwaiter<Http2ServerSession>(*this, stream.id());
        } else {
            Http2FrameHeader header;
            std::string_view payload;
            if (auto readResult = co_await readFrame(header, payload); readResult.shouldStop()) {
                co_return Http2DataWindowResult::stopWriting();
            }
            if (auto processResult = co_await processFrame(header, payload); processResult.shouldStop()) {
                co_return Http2DataWindowResult::stopWriting();
            }
            consumeInput(kHttp2FrameHeaderBytes + header.length);
        }
    }
    co_return (!closing_ && !stream.isReset())
        ? Http2DataWindowResult::ready()
        : Http2DataWindowResult::stopWriting();
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeData(
    Http2StreamState& stream,
    std::string_view first,
    std::string_view second,
    bool endStream) {
    if (stream.isReset()) {
        co_return;
    }
    const auto bodySize = first.size() + second.size();
    std::size_t offset = 0;
    while (offset < bodySize) {
        if (!http2SendWindowAvailable(connectionSendWindow_, stream)) {
            if (auto windowResult = co_await waitForDataWindow(stream); windowResult.shouldStop()) {
                co_return;
            }
        }
        if (closing_ || stream.isReset()) {
            co_return;
        }
        const auto availableWindow = http2AvailableSendWindow(connectionSendWindow_, stream);
        const auto chunk = std::min<std::size_t>(
            {bodySize - offset, availableWindow, peerSettings_.maxFrameSize()});
        const auto last = offset + chunk == bodySize;
        const auto payload = http2SliceTwoPartPayload(first, second, offset, chunk);
        http2ConsumeSendWindow(connectionSendWindow_, stream, chunk);
        co_await writeFramePayload(
            Http2FrameType::kData,
            static_cast<std::uint8_t>(endStream && last ? kHttp2FlagEndStream : 0),
            stream.id(),
            payload.first,
            payload.second);
        offset += chunk;
    }
    if (endStream && bodySize == 0) {
        if (stream.isReset()) {
            co_return;
        }
        co_await writeFramePayload(Http2FrameType::kData, kHttp2FlagEndStream, stream.id(), {});
    }
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeFileBody(
    Http2StreamState& stream,
    ResponseFileBody fileBody,
    bool endStream) {
    if (fileBody.length == 0) {
        co_await writeData(stream, {}, {}, endStream);
        co_return;
    }
    auto input = openResponseFileInput(fileBody);
    if (!input) {
        co_await writeData(stream, {}, {}, true);
        co_return;
    }
    input.seekg(static_cast<std::streamoff>(fileBody.offset), std::ios::beg);
    if (!input) {
        co_await writeData(stream, {}, {}, true);
        co_return;
    }
    std::pmr::string fileChunk(memory_.allocator<char>());
    ensureFileChunkBuffer(fileChunk);
    std::uint64_t remaining = fileBody.length;
    while (remaining > 0 && !closing_ && !stream.isReset()) {
        const auto next = static_cast<std::size_t>(std::min<std::uint64_t>(fileChunk.size(), remaining));
        input.read(fileChunk.data(), static_cast<std::streamsize>(next));
        const auto read = input.gcount();
        if (read <= 0) {
            co_return;
        }
        remaining -= static_cast<std::uint64_t>(read);
        co_await writeData(
            stream,
            std::string_view(fileChunk.data(), static_cast<std::size_t>(read)),
            {},
            endStream && remaining == 0);
    }
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeResponse(
    Http2StreamState& stream,
    const HttpResponse& response,
    bool skipBody) {
    const auto policy = responseWritePolicy(response.status());
    if (stream.isReset()) {
        co_return;
    }
    const bool bodyAllowed = policy.bodyAllowed();
    const bool sendBody = bodyAllowed && !skipBody;
    std::uint64_t contentLength = 0;
    if (bodyAllowed) {
        contentLength = responseHasFileBody(response) ? responseFileBody(response).length : responseBodySize(response);
    }
    appendHttp2ResponseHeaders(stream, response, contentLength);
    const bool endHeadersStream = !sendBody || contentLength == 0;
    co_await writeHeaders(stream, stream.responseHeaderBlock(), endHeadersStream);
    http2ReleaseResponseHeaderBlock(stream);
    if (endHeadersStream) {
        co_return;
    }
    if (responseHasFileBody(response)) {
        co_await writeFileBody(stream, responseFileBody(response), true);
        co_return;
    }
    const auto body = responseBodyBytes(response);
    co_await writeData(stream, body, {}, true);
}
