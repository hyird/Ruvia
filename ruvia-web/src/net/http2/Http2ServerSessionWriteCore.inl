template <typename Stream>
template <typename WriteOperation>
Task<void> Http2ServerSession<Stream>::writeSerialized(WriteOperation operation, bool finalWrite) {
    while (writeInProgress_) {
        if (closing_ && !finalWrite) {
            co_return;
        }
        co_await Http2WriteTurnAwaiter<Http2ServerSession>(*this);
    }
    if (closing_ && !finalWrite) {
        co_return;
    }

    writeInProgress_ = true;
    scannerEntry_.setPhase(ConnectionScanner::Phase::kWriting);
    const auto ec = co_await asyncError([&operation](auto handler) mutable {
        operation(std::move(handler));
    });
    writeInProgress_ = false;
    if (ec) {
        closing_ = true;
        resumeAllWriteWaiters();
        throw std::system_error(ec);
    }
    scannerEntry_.touch();
    resumeNextWriteWaiter();
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeRaw(std::string_view bytes, bool finalWrite) {
    co_await writeSerialized(
        [this, bytes](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(bytes), std::move(handler));
        },
        finalWrite);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::writeFramePayload(
    Http2FrameType type,
    std::uint8_t flags,
    std::uint32_t streamId,
    std::string_view firstPayload,
    std::string_view secondPayload,
    bool finalWrite) {
    const auto payloadSize = firstPayload.size() + secondPayload.size();
    std::array<char, kHttp2FrameHeaderBytes> header;
    http2EncodeFrameHeader(
        header.data(),
        static_cast<std::uint32_t>(payloadSize),
        type,
        flags,
        streamId);
    if (payloadSize == 0) {
        co_await writeSerialized([this, &header](auto handler) mutable {
            asio::async_write(stream_, asio::buffer(header), std::move(handler));
        }, finalWrite);
        co_return;
    }

    if (secondPayload.empty()) {
        const std::array<asio::const_buffer, 2> buffers{
            asio::buffer(header),
            asio::buffer(firstPayload)};
        co_await writeSerialized([this, &buffers](auto handler) mutable {
            asio::async_write(stream_, buffers, std::move(handler));
        }, finalWrite);
        co_return;
    }

    const std::array<asio::const_buffer, 3> buffers{
        asio::buffer(header),
        asio::buffer(firstPayload),
        asio::buffer(secondPayload)};
    co_await writeSerialized([this, &buffers](auto handler) mutable {
        asio::async_write(stream_, buffers, std::move(handler));
    }, finalWrite);
}
