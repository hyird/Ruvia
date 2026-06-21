template <typename Stream>
Task<bool> Http2ServerSession<Stream>::processHeaders(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "HEADERS stream id must be nonzero");
        co_return false;
    }
    if ((header.streamId & 1U) == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid client stream id");
        co_return false;
    }

    std::uint32_t dependency = 0;
    if (!http2HeadersPriorityDependency(header, payload, dependency)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid HEADERS priority");
        co_return false;
    }
    if ((header.flags & kHttp2FlagPriority) != 0 && dependency == header.streamId) {
        if (header.streamId > lastStreamId_) {
            lastStreamId_ = header.streamId;
        }
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
        co_return true;
    }

    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->headersDecoded && !existing->reset) {
            co_return co_await processTrailerHeaders(*existing, header, payload);
        }
        if (existing->reset) {
            if (existing->closeSource == Http2StreamCloseSource::kPeer) {
                co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
                co_return true;
            }
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            co_return false;
        }
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        existing->reset = true;
        existing->closeSource = Http2StreamCloseSource::kLocal;
        co_return true;
    }
    if (header.streamId <= lastStreamId_) {
        const auto source = closedStreams_.source(header.streamId);
        if (source == Http2StreamCloseSource::kPeer) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            co_return true;
        }
        if (source == Http2StreamCloseSource::kLocal) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            co_return false;
        }
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "new stream id lower than previous");
        co_return false;
    }

    auto* stream = createStream(header.streamId);
    if (stream == nullptr) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kRefusedStream);
        co_return true;
    }
    lastStreamId_ = header.streamId;
    stream->endStream = (header.flags & kHttp2FlagEndStream) != 0;

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid HEADERS padding");
        co_return false;
    }
    if (!http2StartHeaderBlock(*stream, fragment)) {
        co_await sendRstStream(stream->id, Http2ErrorCode::kEnhanceYourCalm);
        stream->reset = true;
        co_return true;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = decodeHeaderBlock(*stream);
        if (status != HeaderDecodeStatus::kOk) {
            co_return co_await handleHeaderDecodeFailure(*stream, status);
        }
        queueInitialStreamIfReady(*stream);
    } else {
        headerContinuation_.start(stream->id, false);
    }
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::processTrailerHeaders(
    Http2StreamState& stream,
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (stream.bodyEnded) {
        co_await sendRstStream(stream.id, Http2ErrorCode::kStreamClosed);
        stream.reset = true;
        co_return true;
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        co_await sendRstStream(stream.id, Http2ErrorCode::kProtocolError);
        stream.reset = true;
        co_return true;
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid trailer padding");
        co_return false;
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        co_await sendRstStream(stream.id, Http2ErrorCode::kEnhanceYourCalm);
        stream.reset = true;
        co_return true;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = finishTrailerBlock(stream);
        if (status != HeaderDecodeStatus::kOk) {
            co_return co_await handleHeaderDecodeFailure(stream, status);
        }
    } else {
        headerContinuation_.start(stream.id, true);
    }
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::processContinuation(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (!headerContinuation_.matches(header.streamId)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid CONTINUATION");
        co_return false;
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "missing CONTINUATION stream");
        co_return false;
    }
    if (!http2AppendHeaderBlock(*stream, payload)) {
        co_await sendRstStream(stream->id, Http2ErrorCode::kEnhanceYourCalm);
        stream->reset = true;
        headerContinuation_.reset();
        co_return true;
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const bool trailers = headerContinuation_.finishWasTrailers();
        if (trailers) {
            const auto status = finishTrailerBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                co_return co_await handleHeaderDecodeFailure(*stream, status);
            }
        } else {
            const auto status = decodeHeaderBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                co_return co_await handleHeaderDecodeFailure(*stream, status);
            }
            queueInitialStreamIfReady(*stream);
        }
    }
    co_return true;
}
