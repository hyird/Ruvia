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

template <typename Stream>
Task<void> Http2ServerSession<Stream>::sendConnectionWindowUpdate(std::uint32_t increment) {
    if (increment == 0) {
        co_return;
    }
    std::array<char, kHttp2WindowUpdateFrameBytes> buffer;
    auto* out = http2WriteWindowUpdate(buffer.data(), 0, increment);
    co_await writeRaw(std::string_view(buffer.data(), static_cast<std::size_t>(out - buffer.data())));
}

template <typename Stream>
Task<Http2SessionFlow> Http2ServerSession<Stream>::dropDataFrameKeepConnection(
    std::size_t flowBytes,
    bool windowConsumed) {
    // The peer counted this DATA frame against its connection send window (RFC 9113
    // 6.9.1: DATA is flow-controlled including padding, and is counted even when the
    // frame is in error unless we treat it as a connection error). We are keeping the
    // connection, so we MUST hand the credit back or the peer's send window drains to
    // zero and stalls every stream. Only the connection window is replenished here:
    // the stream is being abandoned, so its window is not advertised.
    if (windowConsumed) {
        connectionReceiveWindow_ += static_cast<std::int32_t>(flowBytes);
    }
    co_await sendConnectionWindowUpdate(static_cast<std::uint32_t>(flowBytes));
    co_return Http2SessionFlow::keepRunning();
}
