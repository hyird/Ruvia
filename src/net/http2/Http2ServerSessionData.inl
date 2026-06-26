template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processData(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA stream id must be nonzero");
        co_return Http2SessionFlow::stopRunning();
    }
    if ((header.streamId & 1U) == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA on invalid client stream id");
        co_return Http2SessionFlow::stopRunning();
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (header.streamId <= lastStreamId_) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            co_return Http2SessionFlow::keepRunning();
        }
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        co_return Http2SessionFlow::stopRunning();
    }
    if (stream->isReset()) {
        co_return Http2SessionFlow::keepRunning();
    }
    if (!stream->headersDecoded()) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        stream->markReset();
        co_return Http2SessionFlow::keepRunning();
    }
    if (stream->bodyEnded()) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        stream->markReset();
        co_return Http2SessionFlow::keepRunning();
    }

    const auto flowBytes = static_cast<std::int32_t>(payload.size());
    switch (http2ConsumeReceiveWindows(connectionReceiveWindow_, *stream, flowBytes)) {
        case Http2ReceiveWindowResult::kOk:
            break;
        case Http2ReceiveWindowResult::kConnectionExceeded:
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFlowControlError, "connection flow-control window exceeded");
            co_return Http2SessionFlow::stopRunning();
        case Http2ReceiveWindowResult::kStreamExceeded:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            stream->markReset();
            co_return Http2SessionFlow::keepRunning();
    }

    std::string_view data;
    if (!http2DecodeDataPayload(header, payload, data)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid DATA padding");
        co_return Http2SessionFlow::stopRunning();
    }

    switch (http2AccountDataBody(*stream, data.size(), options_.maxStreamBodyBytes, options_.maxBufferedBodyBytes)) {
        case Http2BodyAccountingResult::kOk:
            break;
        case Http2BodyAccountingResult::kTooLarge:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kCancel);
            stream->markReset();
            co_return Http2SessionFlow::keepRunning();
        case Http2BodyAccountingResult::kContentLengthExceeded:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            stream->markReset();
            co_return Http2SessionFlow::keepRunning();
    }
    if (flowBytes > 0) {
        const auto increment = static_cast<std::uint32_t>(flowBytes);
        http2RestoreReceiveWindows(connectionReceiveWindow_, *stream, flowBytes);
        co_await sendDataWindowUpdates(header.streamId, increment);
    }

    if (stream->usesStreamRequestBody()) {
        http2EnqueueStreamBodyChunk(*stream, data);
        if ((header.flags & kHttp2FlagEndStream) != 0) {
            if (!http2BodyLengthComplete(*stream)) {
                co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
                stream->markReset();
                co_return Http2SessionFlow::keepRunning();
            }
            http2MarkBodyEnded(*stream);
            if (!stream->dispatchStarted()) {
                queueReady(stream->id());
            }
        }
        resumeBodyWaiter(*stream);
        co_return Http2SessionFlow::keepRunning();
    }

    stream->appendRequestBody(data);
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (!http2BodyLengthComplete(*stream)) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            stream->markReset();
            co_return Http2SessionFlow::keepRunning();
        }
        http2MarkBodyEnded(*stream);
        queueReady(stream->id());
    }
    co_return Http2SessionFlow::keepRunning();
}
