template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processHeaders(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "HEADERS stream id must be nonzero");
        co_return Http2SessionFlow::stopRunning();
    }
    if ((header.streamId & 1U) == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid client stream id");
        co_return Http2SessionFlow::stopRunning();
    }

    std::uint32_t dependency = 0;
    if (!http2HeadersPriorityDependency(header, payload, dependency)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid HEADERS priority");
        co_return Http2SessionFlow::stopRunning();
    }
    if ((header.flags & kHttp2FlagPriority) != 0 && dependency == header.streamId) {
        if (header.streamId > lastStreamId_) {
            lastStreamId_ = header.streamId;
        }
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
        co_return Http2SessionFlow::keepRunning();
    }

    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->headersDecoded() && !existing->isReset()) {
            co_return co_await processTrailerHeaders(*existing, header, payload);
        }
        if (existing->isReset()) {
            if (existing->closeSource() == Http2StreamCloseSource::kPeer) {
                co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
                co_return Http2SessionFlow::keepRunning();
            }
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            co_return Http2SessionFlow::stopRunning();
        }
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        existing->markReset();
        co_return Http2SessionFlow::keepRunning();
    }
    if (header.streamId <= lastStreamId_) {
        const auto source = closedStreams_.source(header.streamId);
        if (source == Http2StreamCloseSource::kPeer) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            co_return Http2SessionFlow::keepRunning();
        }
        if (source == Http2StreamCloseSource::kLocal) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            co_return Http2SessionFlow::stopRunning();
        }
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "new stream id lower than previous");
        co_return Http2SessionFlow::stopRunning();
    }

    // While draining, a stream above the id we advertised in GOAWAY must not be
    // processed (RFC 9113 §6.8); route it through the refused-stream path so its
    // header block is still decoded (to keep HPACK in sync) and then RST'd.
    const bool drainRefused = draining_ && header.streamId > goawayLastStreamId_;
    auto* stream = drainRefused ? nullptr : createStream(header.streamId);
    lastStreamId_ = header.streamId;
    const bool refusedStream = stream == nullptr;
    if (refusedStream) {
        refusedHeaderStream_.emplace(header.streamId, memory_.resource());
        stream = &*refusedHeaderStream_;
    }
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        stream->markPeerEndStream();
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid HEADERS padding");
        co_return Http2SessionFlow::stopRunning();
    }
    if (!http2StartHeaderBlock(*stream, fragment)) {
        co_await sendRstStream(stream->id(), Http2ErrorCode::kEnhanceYourCalm);
        stream->markReset();
        co_return Http2SessionFlow::keepRunning();
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeHeaderBlock(*stream);
        if (status != HeaderDecodeStatus::kOk) {
            if (refusedStream) {
                refusedHeaderStream_.reset();
            }
            co_return co_await handleHeaderDecodeFailure(*stream, status);
        }
        if (refusedStream) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kRefusedStream);
            closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
            refusedHeaderStream_.reset();
        } else {
            queueInitialStreamIfReady(*stream);
        }
    } else {
        headerContinuation_.start(stream->id(), false);
    }
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processTrailerHeaders(
    Http2StreamState& stream,
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (stream.bodyEnded()) {
        co_await sendRstStream(stream.id(), Http2ErrorCode::kStreamClosed);
        stream.markReset();
        co_return Http2SessionFlow::keepRunning();
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        co_await sendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        stream.markReset();
        co_return Http2SessionFlow::keepRunning();
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid trailer padding");
        co_return Http2SessionFlow::stopRunning();
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        co_await sendRstStream(stream.id(), Http2ErrorCode::kEnhanceYourCalm);
        stream.markReset();
        co_return Http2SessionFlow::keepRunning();
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = finishTrailerBlock(stream);
        if (status != HeaderDecodeStatus::kOk) {
            co_return co_await handleHeaderDecodeFailure(stream, status);
        }
    } else {
        headerContinuation_.start(stream.id(), true);
    }
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processContinuation(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (!headerContinuation_.matches(header.streamId)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid CONTINUATION");
        co_return Http2SessionFlow::stopRunning();
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (!refusedHeaderStream_ || refusedHeaderStream_->id() != header.streamId) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "missing CONTINUATION stream");
            co_return Http2SessionFlow::stopRunning();
        }
        stream = &*refusedHeaderStream_;
    }
    if (!http2AppendHeaderBlock(*stream, payload)) {
        co_await sendRstStream(stream->id(), Http2ErrorCode::kEnhanceYourCalm);
        stream->markReset();
        headerContinuation_.reset();
        if (refusedHeaderStream_ && refusedHeaderStream_->id() == stream->id()) {
            refusedHeaderStream_.reset();
        }
        co_return Http2SessionFlow::keepRunning();
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const bool trailers = headerContinuation_.finishWasTrailers();
        if (trailers) {
            const auto status = finishTrailerBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                co_return co_await handleHeaderDecodeFailure(*stream, status);
            }
        } else {
            const bool refusedStream = refusedHeaderStream_ && refusedHeaderStream_->id() == stream->id();
            const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeHeaderBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                if (refusedStream) {
                    refusedHeaderStream_.reset();
                }
                co_return co_await handleHeaderDecodeFailure(*stream, status);
            }
            if (refusedStream) {
                co_await sendRstStream(stream->id(), Http2ErrorCode::kRefusedStream);
                closedStreams_.remember(stream->id(), Http2StreamCloseSource::kLocal);
                refusedHeaderStream_.reset();
            } else {
                queueInitialStreamIfReady(*stream);
            }
        }
    }
    co_return Http2SessionFlow::keepRunning();
}
