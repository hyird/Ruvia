template <typename Stream>
Http2StreamState* Http2ServerSession<Stream>::findStream(std::uint32_t streamId) noexcept {
    return http2FindStream(streams_, streamId);
}

template <typename Stream>
bool Http2ServerSession<Stream>::isIdleStream(std::uint32_t streamId) const noexcept {
    return http2IsIdleStream(streamId, lastStreamId_);
}

template <typename Stream>
Http2StreamState* Http2ServerSession<Stream>::createStream(std::uint32_t streamId) {
    return http2CreateStream(streams_, streamId, memory_.resource(), peerSettings_.initialWindowSize());
}

template <typename Stream>
void Http2ServerSession<Stream>::eraseStreamAt(std::size_t index) noexcept {
    http2EraseStreamAt(streams_, index);
}

template <typename Stream>
void Http2ServerSession<Stream>::removeStream(std::uint32_t streamId) noexcept {
    (void)http2RemoveStream(streams_, streamId);
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
    stream->reset = true;
    stream->bodyEnded = true;
    stream->closeSource = source;
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
    for (std::size_t i = 0; i < streams_.size();) {
        if (!streams_[i].reset) {
            ++i;
            continue;
        }
        const auto streamId = streams_[i].id;
        closedStreams_.remember(streamId, streams_[i].closeSource == Http2StreamCloseSource::kNone
            ? Http2StreamCloseSource::kLocal
            : streams_[i].closeSource);
        removeReadyStream(streamId);
        eraseStreamAt(i);
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::queueReady(std::uint32_t streamId) {
    if (auto* stream = findStream(streamId); stream != nullptr && !stream->queued && !stream->reset) {
        stream->queued = true;
        readyQueue_.push(streamId);
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
    if (stream.dispatchStarted || stream.reset) {
        stream.queued = false;
        return;
    }
    stream.queued = false;
    stream.dispatchStarted = true;
    ++activeDispatches_;
    asio::co_spawn(
        socket_.get_executor(),
        taskAsAwaitable(dispatchStreamTask(stream.id)),
        asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::dispatchStreamTask(std::uint32_t streamId) {
    try {
        auto* stream = findStream(streamId);
        if (stream != nullptr && !stream->reset) {
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
        stream->reset = true;
        stream->bodyEnded = true;
        stream->closeSource = Http2StreamCloseSource::kLocal;
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
    for (auto& stream : streams_) {
        resumeBodyWaiter(stream);
    }
}

template <typename Stream>
void Http2ServerSession<Stream>::resumeSendWindowWaiters() {
    sendWindowWaiters_.resumeAllCurrent();
}

template <typename Stream>
Task<std::optional<std::string_view>> Http2ServerSession<Stream>::readBodyChunk(std::uint32_t streamId) {
    for (;;) {
        auto* stream = findStream(streamId);
        if (stream == nullptr || stream->reset) {
            co_return std::nullopt;
        }
        if (auto chunk = http2PopStreamBodyChunk(*stream); !chunk.empty()) {
            co_return chunk;
        }
        if (http2HasQueuedStreamBodyChunk(*stream)) {
            continue;
        }
        if (stream->bodyEnded) {
            co_return std::nullopt;
        }
        co_await Http2BodyChunkAwaiter<Http2ServerSession>(*this, streamId);
    }
}
