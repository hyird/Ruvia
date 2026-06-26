template <typename Stream>
Http2ServerSession<Stream>::Http2ServerSession(
    Stream& stream,
    asio::ip::tcp::socket& socket,
    WorkerMemory& memory,
    const RouteTable& routes,
    DbRegistry* databases,
    RedisRegistry* redis,
    HttpClientRegistry* httpClients,
    const HttpServerOptions& options,
    ConnectionScanner::Entry& scannerEntry,
    std::string_view remoteAddress,
    RateLimiter* rateLimiter,
    std::string_view clientCertificate,
    const std::atomic_bool* serverStarted)
    : stream_(stream),
      socket_(socket),
      memory_(memory),
      routes_(routes),
      databases_(databases),
      redis_(redis),
      httpClients_(httpClients),
      options_(options),
      scannerEntry_(scannerEntry),
      remoteAddress_(remoteAddress),
      rateLimiter_(rateLimiter),
      clientCertificate_(clientCertificate),
      serverStarted_(serverStarted),
      input_(memory.resource()),
      streams_(memory.resource()),
      writeWaiters_(memory.resource()),
      sendWindowWaiters_(memory.resource()),
      decoder_(memory.resource()) {
    decoder_.setMaxDynamicTableSize(4096);
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::run(std::string_view initialBytes) {
    input_.assign(initialBytes.data(), initialBytes.size());
    inputOffset_ = 0;
    scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingHeader);
    if (!(co_await readPreface())) {
        closing_ = true;
        co_return;
    }
    co_await sendLocalSettings();
    co_await runFrameLoop();
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::runUpgraded(
    const HttpServerParseResult& parsed,
    std::string_view settingsPayload,
    std::string_view body,
    std::string_view initialBytes) {
    input_.assign(initialBytes.data(), initialBytes.size());
    inputOffset_ = 0;
    scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingHeader);
    if (!(co_await applySettingsPayload(settingsPayload))) {
        closing_ = true;
        co_return;
    }
    receivedFirstSettings_ = true;
    if (!seedUpgradedStream(parsed, body)) {
        co_await sendGoaway(1, Http2ErrorCode::kProtocolError, "invalid upgraded request");
        co_return;
    }
    co_await sendLocalSettings();
    co_await sendSettingsAck();
    if (!(co_await readPreface())) {
        closing_ = true;
        co_return;
    }
    receivedFirstSettings_ = false;
    co_await runFrameLoop();
}

template <typename Stream>
Task<void> Http2ServerSession<Stream>::runFrameLoop() {
    readerRunning_ = true;
    while (!closing_) {
        Http2FrameHeader header;
        std::string_view payload;
        if (!(co_await readFrame(header, payload))) {
            break;
        }
        // The server has begun draining: tell the peer to stop opening streams
        // (RFC 9113 §6.8). Streams already started keep running; new ones (id
        // above this point) are refused below in processHeaders.
        if (!draining_ && serverStarted_ != nullptr &&
            !serverStarted_->load(std::memory_order_relaxed)) {
            draining_ = true;
            goawayLastStreamId_ = lastStreamId_;
            co_await sendGoaway(lastStreamId_, Http2ErrorCode::kNoError, "server draining");
        }
        if (!(co_await processFrame(header, payload))) {
            break;
        }
        consumeInput(kHttp2FrameHeaderBytes + header.length);
        while (hasReadyStream() && !closing_) {
            const auto streamId = popReadyStream();
            if (auto* stream = findStream(streamId); stream != nullptr && !stream->isReset()) {
                launchStreamDispatch(*stream);
            }
        }
        cleanupClosedStreams();
    }
    readerRunning_ = false;
    closing_ = true;
    resumeAllBodyWaiters();
    resumeSendWindowWaiters();
    if (!writeInProgress_) {
        resumeAllWriteWaiters();
    }
    co_await Http2DispatchDrainAwaiter<Http2ServerSession>(*this);
    cleanupClosedStreams();
}
