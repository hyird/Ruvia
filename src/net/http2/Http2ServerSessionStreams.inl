template <typename Stream>
Http2StreamState* Http2ServerSession<Stream>::findStream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

template <typename Stream>
bool Http2ServerSession<Stream>::isIdleStream(std::uint32_t streamId) const noexcept {
    return http2IsIdleStream(streamId, lastStreamId_);
}

template <typename Stream>
Http2StreamState* Http2ServerSession<Stream>::createStream(std::uint32_t streamId) {
    return streams_.create(streamId, peerSettings_.initialWindowSize());
}

template <typename Stream>
void Http2ServerSession<Stream>::removeStream(std::uint32_t streamId) noexcept {
    (void)streams_.remove(streamId);
}

template <typename Stream>
void Http2ServerSession<Stream>::closeStream(
    std::uint32_t streamId,
    Http2StreamCloseSource source) {
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        removeReadyStream(streamId);
        closedStreams_.remember(streamId, source);
        return;
    }
    stream->markClosed(source);
    removeReadyStream(streamId);
    resumeBodyWaiter(*stream);
    if (dispatchDepth_ == 0) {
        closedStreams_.remember(streamId, source);
        removeStream(streamId);
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::removeReadyStream(std::uint32_t streamId) noexcept {
    readyQueue_.remove(streamId);
}

template <typename Stream>
void Http2ServerSession<Stream>::cleanupClosedStreams() noexcept {
    if (dispatchDepth_ != 0) {
        return;
    }
    streams_.removeReset([this](const Http2StreamState& stream) noexcept {
        const auto streamId = stream.id();
        const auto closeSource = stream.closeSource();
        closedStreams_.remember(streamId, closeSource == Http2StreamCloseSource::kNone
            ? Http2StreamCloseSource::kLocal
            : closeSource);
        removeReadyStream(streamId);
    });
}

template <typename Stream>
void Http2ServerSession<Stream>::queueReady(std::uint32_t streamId) {
    if (auto* stream = findStream(streamId); stream != nullptr && stream->tryMarkQueued()) {
        if (readyQueue_.push(streamId)) {
            return;
        }
        stream->clearQueued();
    }
}

template <typename Stream>
bool Http2ServerSession<Stream>::hasReadyStream() const noexcept {
    return readyQueue_.hasReady();
}

template <typename Stream>
std::uint32_t Http2ServerSession<Stream>::popReadyStream() noexcept {
    return readyQueue_.pop();
}

template <typename Stream>
void Http2ServerSession<Stream>::launchStreamDispatch(Http2StreamState& stream) {
    if (!stream.tryStartDispatch()) {
        return;
    }
    ++activeDispatches_;
    asio::co_spawn(
        socket_.get_executor(),
        taskAsAwaitable(dispatchStreamTask(stream.id())),
        asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::dispatchStreamTask(std::uint32_t streamId) {
    try {
        auto* stream = findStream(streamId);
        if (stream != nullptr && !stream->isReset()) {
            Http2DispatchGuard<Http2ServerSession> dispatchGuard(*this);
            co_await dispatchStream(*stream);
        }
    } catch (...) {
        closing_ = true;
        resumeAllBodyWaiters();
        resumeSendWindowWaiters();
        if (!writeInProgress_) {
            resumeAllWriteWaiters();
        }
    }
    finishStreamDispatch(streamId);
}

template <typename Stream>
void Http2ServerSession<Stream>::finishStreamDispatch(std::uint32_t streamId) noexcept {
    if (auto* stream = findStream(streamId); stream != nullptr) {
        stream->markClosed(Http2StreamCloseSource::kLocal);
        removeReadyStream(streamId);
        resumeBodyWaiter(*stream);
    } else {
        removeReadyStream(streamId);
    }
    if (activeDispatches_ > 0) {
        --activeDispatches_;
    }
    if (dispatchDepth_ == 0) {
        cleanupClosedStreams();
    }
    if (activeDispatches_ == 0 && dispatchDrainWaiter_) {
        auto continuation = std::exchange(dispatchDrainWaiter_, {});
        continuation.resume();
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeNextWriteWaiter() {
    writeWaiters_.resumeNext();
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeAllWriteWaiters() {
    writeWaiters_.resumeAll();
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeBodyWaiter(Http2StreamState& stream) noexcept {
    auto continuation = http2TakeBodyWaiter(stream);
    if (continuation) {
        continuation.resume();
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeAllBodyWaiters() noexcept {
    streams_.forEach([this](Http2StreamState& stream) noexcept {
        resumeBodyWaiter(stream);
    });
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeSendWindowWaiters() {
    sendWindowWaiters_.resumeAllCurrent();
}

template <typename Stream>
Task<std::optional<std::string_view>> Http2ServerSession<Stream>::readBodyChunk(std::uint32_t streamId) {
    for (;;) {
        auto* stream = findStream(streamId);
        if (stream == nullptr || stream->isReset()) {
            co_return std::nullopt;
        }
        if (auto chunk = http2PopStreamBodyChunk(*stream); !chunk.empty()) {
            co_return chunk;
        }
        if (http2HasQueuedStreamBodyChunk(*stream)) {
            continue;
        }
        if (stream->bodyEnded()) {
            co_return std::nullopt;
        }
        co_await Http2BodyChunkAwaiter<Http2ServerSession>(*this, streamId);
    }
}
