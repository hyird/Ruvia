template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processFrame(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (!receivedFirstSettings_ && header.type != static_cast<std::uint8_t>(Http2FrameType::kSettings)) {
        co_await sendGoaway(0, Http2ErrorCode::kProtocolError, "first frame must be SETTINGS");
        co_return Http2SessionFlow::stopRunning();
    }

    if (!headerContinuation_.expectsFrameType(header.type)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "expected CONTINUATION");
        co_return Http2SessionFlow::stopRunning();
    }

    switch (static_cast<Http2FrameType>(header.type)) {
        case Http2FrameType::kData:
            co_return co_await processData(header, payload);
        case Http2FrameType::kHeaders:
            co_return co_await processHeaders(header, payload);
        case Http2FrameType::kPriority:
            co_return co_await processPriority(header, payload);
        case Http2FrameType::kRstStream:
            co_return co_await processRstStream(header, payload);
        case Http2FrameType::kSettings:
            co_return co_await processSettings(header, payload);
        case Http2FrameType::kPushPromise:
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "client PUSH_PROMISE is invalid");
            co_return Http2SessionFlow::stopRunning();
        case Http2FrameType::kPing:
            co_return co_await processPing(header, payload);
        case Http2FrameType::kGoaway:
            closing_ = true;
            co_return Http2SessionFlow::stopRunning();
        case Http2FrameType::kWindowUpdate:
            co_return co_await processWindowUpdate(header, payload);
        case Http2FrameType::kContinuation:
            co_return co_await processContinuation(header, payload);
    }
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processSettings(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId != 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "SETTINGS stream id must be zero");
        co_return Http2SessionFlow::stopRunning();
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        if (!payload.empty()) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "SETTINGS ack payload");
            co_return Http2SessionFlow::stopRunning();
        }
        receivedFirstSettings_ = true;
        co_return Http2SessionFlow::keepRunning();
    }
    if (auto settingsResult = co_await applySettingsPayload(payload); settingsResult.shouldStop()) {
        co_return settingsResult;
    }
    receivedFirstSettings_ = true;
    co_await sendSettingsAck();
    resumeSendWindowWaiters();
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::applySettingsPayload(std::string_view payload) {
    if (!http2SettingsPayloadSizeValid(payload)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "invalid SETTINGS size");
        co_return Http2SessionFlow::stopRunning();
    }

    for (std::size_t offset = 0; offset < payload.size(); offset += 6) {
        const auto entry = http2ReadSettingEntry(payload, offset);
        const auto result = peerSettings_.apply(entry.id, entry.value);
        if (result.status != Http2PeerSettingsStatus::kOk) {
            co_await sendGoaway(
                lastStreamId_,
                http2PeerSettingsErrorCode(result.status),
                http2PeerSettingsErrorMessage(result.status));
            co_return Http2SessionFlow::stopRunning();
        }
        if (result.initialWindowChanged &&
            !http2ApplyStreamSendWindowDelta(streams_, result.initialWindowDelta)) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFlowControlError, "stream window overflow");
            co_return Http2SessionFlow::stopRunning();
        }
    }
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processPing(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (payload.size() != 8) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "invalid PING");
        co_return Http2SessionFlow::stopRunning();
    }
    if (header.streamId != 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "PING stream id must be zero");
        co_return Http2SessionFlow::stopRunning();
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        co_return Http2SessionFlow::keepRunning();
    }
    co_await writeFramePayload(Http2FrameType::kPing, kHttp2FlagAck, 0, payload);
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processPriority(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (payload.size() != 5) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "invalid PRIORITY");
        co_return Http2SessionFlow::stopRunning();
    }
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "PRIORITY stream id must be nonzero");
        co_return Http2SessionFlow::stopRunning();
    }
    const auto dependency = http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
    if (dependency == header.streamId) {
        co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(header.streamId);
        co_return Http2SessionFlow::keepRunning();
    }
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processRstStream(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (payload.size() != 4) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "invalid RST_STREAM");
        co_return Http2SessionFlow::stopRunning();
    }
    if (header.streamId == 0) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "RST_STREAM stream id must be nonzero");
        co_return Http2SessionFlow::stopRunning();
    }
    if (findStream(header.streamId) == nullptr && isIdleStream(header.streamId)) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "RST_STREAM on idle stream");
        co_return Http2SessionFlow::stopRunning();
    }
    closeStream(header.streamId, Http2StreamCloseSource::kPeer);
    co_return Http2SessionFlow::keepRunning();
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::processWindowUpdate(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (payload.size() != 4) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "invalid WINDOW_UPDATE");
        co_return Http2SessionFlow::stopRunning();
    }
    const auto increment = http2WindowUpdateIncrement(payload);
    if (header.streamId == 0) {
        switch (http2ApplyWindowUpdate(connectionSendWindow_, increment)) {
            case Http2WindowUpdateResult::kOk:
                resumeSendWindowWaiters();
                co_return Http2SessionFlow::keepRunning();
            case Http2WindowUpdateResult::kZeroIncrement:
                co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "zero connection WINDOW_UPDATE");
                co_return Http2SessionFlow::stopRunning();
            case Http2WindowUpdateResult::kOverflow:
                co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFlowControlError, "connection window overflow");
                co_return Http2SessionFlow::stopRunning();
        }
        co_return Http2SessionFlow::keepRunning();
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (isIdleStream(header.streamId)) {
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kProtocolError, "WINDOW_UPDATE on idle stream");
            co_return Http2SessionFlow::stopRunning();
        }
        co_return Http2SessionFlow::keepRunning();
    }
    switch (http2ApplyStreamWindowUpdate(*stream, increment)) {
        case Http2WindowUpdateResult::kOk:
            resumeSendWindowWaiters();
            co_return Http2SessionFlow::keepRunning();
        case Http2WindowUpdateResult::kZeroIncrement:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            stream->markReset();
            resumeSendWindowWaiters();
            co_return Http2SessionFlow::keepRunning();
        case Http2WindowUpdateResult::kOverflow:
            co_await sendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            stream->markReset();
            resumeSendWindowWaiters();
            co_return Http2SessionFlow::keepRunning();
    }
    co_return Http2SessionFlow::keepRunning();
}
