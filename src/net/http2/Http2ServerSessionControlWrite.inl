template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendLocalSettings() {
    controlWriteBuffer_.clear();
    controlWriteBuffer_.reserve(kHttp2LocalSettingsFrameBytes + kHttp2WindowUpdateFrameBytes);
    http2AppendLocalSettingsFrame(controlWriteBuffer_);
    if constexpr (kHttp2LocalInitialWindowSize > kHttp2DefaultInitialWindowSize) {
        http2AppendWindowUpdate(
            controlWriteBuffer_,
            0,
            kHttp2LocalInitialWindowSize - static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
    }
    co_await writeRaw(controlWriteBuffer_);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendSettingsAck() {
    controlWriteBuffer_.clear();
    http2AppendFrameHeader(controlWriteBuffer_, 0, Http2FrameType::kSettings, kHttp2FlagAck, 0);
    co_await writeRaw(controlWriteBuffer_);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendGoaway(
    std::uint32_t lastStreamId,
    Http2ErrorCode error,
    std::string_view debug) {
    controlWriteBuffer_.clear();
    http2AppendGoaway(controlWriteBuffer_, lastStreamId, error, debug);
    closing_ = true;
    co_await writeRaw(controlWriteBuffer_, true);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendRstStream(
    std::uint32_t streamId,
    Http2ErrorCode error) {
    controlWriteBuffer_.clear();
    http2AppendRstStream(controlWriteBuffer_, streamId, error);
    co_await writeRaw(controlWriteBuffer_);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendDataWindowUpdates(
    std::uint32_t streamId,
    std::uint32_t increment) {
    if (increment == 0) {
        co_return;
    }
    controlWriteBuffer_.clear();
    http2AppendDataWindowUpdates(controlWriteBuffer_, streamId, increment);
    co_await writeRaw(controlWriteBuffer_);
}
