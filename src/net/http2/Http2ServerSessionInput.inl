template <typename Stream>
std::size_t Http2ServerSession<Stream>::availableInput() const noexcept {
    return http2AvailableInput(input_, inputOffset_);
}

template <typename Stream>
std::string_view Http2ServerSession<Stream>::inputView(std::size_t size) const noexcept {
    return http2InputView(input_, inputOffset_, size);
}

template <typename Stream>
void Http2ServerSession<Stream>::consumeInput(std::size_t size) {
    http2ConsumeInput(input_, inputOffset_, size);
    http2ReclaimDrainedInput(input_);
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::ensureInput(std::size_t size) {
    while (availableInput() < size) {
        const auto oldSize = input_.size();
        resizePmrStringForOverwrite(input_, oldSize + kReadChunkBytes);
        auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, oldSize](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(input_.data() + oldSize, input_.size() - oldSize),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            co_return false;
        }
        input_.resize(oldSize + bytesRead);
        scannerEntry_.touch();
    }
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::readPreface() {
    if (!(co_await ensureInput(kHttp2ClientPreface.size()))) {
        co_return false;
    }
    if (inputView(kHttp2ClientPreface.size()) != kHttp2ClientPreface) {
        co_await sendGoaway(0, Http2ErrorCode::kProtocolError, "invalid connection preface");
        co_return false;
    }
    consumeInput(kHttp2ClientPreface.size());
    co_return true;
}

template <typename Stream>
Task<bool> Http2ServerSession<Stream>::readFrame(
    Http2FrameHeader& header,
    std::string_view& payload) {
    scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingHeader);
    if (!(co_await ensureInput(kHttp2FrameHeaderBytes))) {
        co_return false;
    }
    header = http2ParseFrameHeader(inputView(kHttp2FrameHeaderBytes));
    if (header.length > kHttp2MaxFrameSizeLimit || header.length > localMaxFrameSize_) {
        co_await sendGoaway(lastStreamId_, Http2ErrorCode::kFrameSizeError, "frame too large");
        co_return false;
    }
    if (!(co_await ensureInput(kHttp2FrameHeaderBytes + header.length))) {
        co_return false;
    }
    payload = std::string_view(input_.data() + inputOffset_ + kHttp2FrameHeaderBytes, header.length);
    co_return true;
}
