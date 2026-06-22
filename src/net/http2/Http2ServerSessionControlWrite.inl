template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendLocalSettings() {
    std::array<char, kHttp2LocalSettingsFrameBytes + kHttp2WindowUpdateFrameBytes> buffer;
    auto* out = http2WriteLocalSettingsFrame(buffer.data());
    if constexpr (kHttp2LocalInitialWindowSize > kHttp2DefaultInitialWindowSize) {
        out = http2WriteWindowUpdate(
            out,
            0,
            kHttp2LocalInitialWindowSize - static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
    }
    co_await writeRaw(std::string_view(buffer.data(), static_cast<std::size_t>(out - buffer.data())));
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendSettingsAck() {
    co_await writeFramePayload(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendGoaway(
    std::uint32_t lastStreamId,
    Http2ErrorCode error,
    std::string_view debug) {
    std::array<char, 8> payload;
    auto* out = http2WriteGoawayPayload(payload.data(), lastStreamId, error);
    closing_ = true;
    co_await writeFramePayload(
        Http2FrameType::kGoaway,
        0,
        0,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())),
        debug,
        true);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendRstStream(
    std::uint32_t streamId,
    Http2ErrorCode error) {
    std::array<char, 4> payload;
    auto* out = http2Write32(payload.data(), static_cast<std::uint32_t>(error));
    co_await writeFramePayload(
        Http2FrameType::kRstStream,
        0,
        streamId,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())));
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendDataWindowUpdates(
    std::uint32_t streamId,
    std::uint32_t increment) {
    if (increment == 0) {
        co_return;
    }
    std::array<char, kHttp2WindowUpdateFrameBytes * 2> buffer;
    auto* out = http2WriteDataWindowUpdates(buffer.data(), streamId, increment);
    co_await writeRaw(std::string_view(buffer.data(), static_cast<std::size_t>(out - buffer.data())));
}
