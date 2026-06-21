template <typename Stream>
Task<bool> Http2ServerSession<Stream>::processData(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA stream id must be nonzero");
        co_return false;
    }
    if ((header.streamId & 1U) == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA on invalid client stream id");
        co_return false;
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (header.streamId <= lastStreamId_) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            co_return true;
        }
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        co_return false;
    }
    if (stream->reset) {
        co_return true;
    }
    if (!stream->headersDecoded) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        stream->reset = true;
        co_return true;
    }
    if (stream->bodyEnded) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        stream->reset = true;
        co_return true;
    }

    const auto flowBytes = static_cast<std::int32_t>(payload.size());
    switch (http2ConsumeReceiveWindows(connectionReceiveWindow_, *stream, flowBytes)) {
        case Http2ReceiveWindowResult::kOk:
            break;
        case Http2ReceiveWindowResult::kConnectionExceeded:
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFlowControlError, "connection flow-control window exceeded");
            co_return false;
        case Http2ReceiveWindowResult::kStreamExceeded:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            stream->reset = true;
            co_return true;
    }

    std::string_view data;
    if (!http2DecodeDataPayload(header, payload, data)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "invalid DATA padding");
        co_return false;
    }

    switch (http2AccountDataBody(*stream, data.size(), options_.maxStreamBodyBytes, options_.maxBufferedBodyBytes)) {
        case Http2BodyAccountingResult::kOk:
            break;
        case Http2BodyAccountingResult::kTooLarge:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kCancel);
            stream->reset = true;
            co_return true;
        case Http2BodyAccountingResult::kContentLengthExceeded:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            stream->reset = true;
            co_return true;
    }
    if (flowBytes > 0) {
        const auto increment = static_cast<std::uint32_t>(flowBytes);
        http2RestoreReceiveWindows(connectionReceiveWindow_, *stream, flowBytes);
        co_await sendDataWindowUpdates(header.streamId, increment);
    }

    if (stream->bodyMode == RequestBodyMode::kStream) {
        http2EnqueueStreamBodyChunk(*stream, data);
        if ((header.flags & kHttp2FlagEndStream) != 0) {
            if (!http2BodyLengthComplete(*stream)) {
                co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
                stream->reset = true;
                co_return true;
            }
            http2MarkBodyEnded(*stream);
            if (!stream->dispatchStarted) {
                queueReady(stream->id);
            }
        }
        resumeBodyWaiter(*stream);
        co_return true;
    }

    stream->body.append(data.data(), data.size());
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (!http2BodyLengthComplete(*stream)) {
            co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            stream->reset = true;
            co_return true;
        }
        http2MarkBodyEnded(*stream);
        queueReady(stream->id);
    }
    co_return true;
}
