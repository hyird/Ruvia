template <typename Stream>
Task<void> HttpServer::handleStreamSession(Stream& stream, TcpSocket& socket, std::string_view clientCertificate) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, keep-alive counters, the remote address, and the count of buffered
    // bytes. The heavy per-request working set (read buffer, request arena,
    // parse result, response head, file chunk) is borrowed from a per-worker
    // pool only while the connection is actively serving and returned the moment
    // it goes idle, so an idle keep-alive connection holds none of it.
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);
    const auto& routes = routes_;
    std::pmr::string remoteAddress(memory_.allocator<char>());
    std::error_code remoteEc;
    const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
    if (!remoteEc) {
        assignRemoteAddress(remoteAddress, remoteEndpoint.address());
    }
    std::size_t requestCount = 0;
    std::size_t usedBytes = 0;
    ConnectionWorkSet* workSet = nullptr;
    WorkSetReturn workSetReturn(workSetPool_, workSet);

    constexpr bool kPlainTcp = std::is_same_v<std::remove_cvref_t<Stream>, TcpSocket>;
    const auto baseRouteServices = ContextServices(&databases_, &redis_, rateLimiter_)
        .withTransport(remoteAddress, clientCertificate, !kPlainTcp);

    for (;;) {
        scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);

        // Borrow-on-use / return-on-idle for the whole work set: when the
        // connection has no buffered bytes and nothing is pending, return the
        // work set to the per-worker pool and wait for readability without
        // holding one, so an idle keep-alive connection occupies no work set
        // (memory scales with in-flight requests, not total connections).
        // available() gates this, so a back-to-back / pipelined burst keeps its
        // work set and pays no extra wait. Plain TCP only: a TLS engine may
        // buffer a decrypted record the raw socket's available() cannot see, so
        // a bufferless wait there could block forever; TLS holds across the
        // connection.
        if constexpr (kPlainTcp) {
            if (plainTcpShouldWaitForNextRequest(socket, usedBytes)) {
                releaseIdleWorkSet(workSetPool_, workSet);
                const auto waitEc = co_await waitForPlainTcpReadable(socket, scannerEntry);
                if (waitEc || !started_.load(std::memory_order_relaxed)) {
                    co_return;
                }
            }
        }
        if (workSet == nullptr) {
            workSet = workSetPool_.acquire();
        }
        auto& readBuffer = workSet->readBuffer;
        auto& parser = workSet->parser;
        auto& parsed = workSet->parsed;
        auto& responseHead = workSet->responseHead;
        auto& fileChunk = workSet->fileChunk;
        auto& compressionScratch = workSet->compressionScratch;
        auto& routeResolution = workSet->routeResolution;

        std::optional<RequestMemory> requestMemoryStorage;
        auto& requestMemory = emplaceRequestMemory(
            requestMemoryStorage,
            memory_,
            std::span<std::byte>(workSet->arenaBlock, sizeof(workSet->arenaBlock)));
        HttpResponse response(requestMemory.resource());
        auto connectionPlan = Http1ServerConnectionPlan::http11Close();
        bool responseStreamDispatched = false;
        bool bufferAlreadyCompacted = false;
        std::size_t consumedBytes = 0;
        std::size_t headerSearchOffset = 0;
        const auto requestStart = std::chrono::steady_clock::now();
        for (;;) {
            if constexpr (kPlainTcp) {
                if (usedBytes > 0) {
                    const auto h2Result = co_await dispatchCleartextHttp2Preface(
                        stream,
                        socket,
                        memory_,
                        routes_,
                        databases_,
                        redis_,
                        options_,
                        scannerEntry,
                        remoteAddress,
                        rateLimiter_,
                        readBuffer,
                        usedBytes,
                        &started_);
                    if (h2Result == CleartextHttp2DispatchResult::kSessionFinished) {
                        co_return;
                    }
                    if (h2Result == CleartextHttp2DispatchResult::kContinueReadLoop) {
                        continue;
                    }
                }
            }
            const auto bufferView = std::string_view(readBuffer.data(), usedBytes);
            parser.parseHead(bufferView, parsed, headerSearchOffset);
            HttpRequestAccess::setResource(parsed.request, requestMemory.resource());
            if (parsed.headReady()) {
                // Reset phase so clientHeaderTimeout stops counting against dispatch
                // time. Body readers will set kReadingPayload on their own; the
                // streaming/websocket paths set their own phases below; the
                // buffered write path sets kWriting before responding. Until
                // one of those transitions, keepaliveTimeout governs as the
                // deadman switch for hung handlers.
                scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
                if (parsed.bodyPlan.expectationAction() ==
                    HttpServerExpectationAction::kUnsupported) {
                    // Expect extensions are valid HTTP syntax. The protocol parser
                    // reports the semantic fact; this Web product deliberately does
                    // not implement extensions beyond 100-continue and chooses the
                    // RFC 9110-permitted 417 response before reading request content.
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo(417, {}, "unsupported Expect header"),
                        baseRouteServices);
                    connectionPlan = http1FinalizeResponseConnection(
                        response, parsed.connectionPlan.requireClose());
                    break;
                }
                if (options_.autoHttps.enabled) {
                    consumedBytes = parsed.headerBytes;
                    if (requestKnownHeader(parsed.request, RequestKnownHeader::kHost).empty()) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo(400, {}, "missing Host header"),
                            baseRouteServices);
                    } else {
                        response = makeAutoHttpsRedirectResponse(
                            parsed.request,
                            requestMemory,
                            options_.autoHttps.httpsPort);
                    }
                    connectionPlan = http1FinalizeResponseConnection(
                        response,
                        parsed.connectionPlan.requireClose());
                    scannerEntry.touch();
                    break;
                }
                routeResolution = routes.resolve(parsed.request);
                const auto appRateLimit = rateLimitRequestAllowed(rateLimiter_, remoteAddress);
                if (!appRateLimit.allowed) {
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo(429, {}, "rate limit exceeded"),
                        baseRouteServices);
                    setRetryAfterSeconds(response, std::chrono::milliseconds(appRateLimit.resetAfterMs));
                    connectionPlan = http1FinalizeResponseConnection(
                        response, parsed.connectionPlan.requireClose());
                    break;
                }
                const auto* resolved = routeResolution.resolved();
                if (resolved == nullptr) {
                    consumedBytes = parsed.headerBytes;
                    if (contentLengthExceedsLimit(
                            parsed.bodyPlan,
                            options_.maxBufferedBodyBytes)) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo(413, {}, "request body is too large"),
                            baseRouteServices);
                        connectionPlan = http1FinalizeResponseConnection(
                            response, parsed.connectionPlan.requireClose());
                        break;
                    }
                    if (auto documentResponse = tryDocumentRootResponse(parsed.request, requestMemory)) {
                        response = std::move(*documentResponse);
                        connectionPlan = http1ApplyRequestBodyConsumption(
                            parsed.connectionPlan,
                            parsed.bodyPlan.requiresConsumption()
                                ? Http1RequestBodyConsumption::kIncomplete
                                : Http1RequestBodyConsumption::kComplete);
                        connectionPlan = finalizeBufferedRouteResponse(
                            response,
                            connectionPlan,
                            requestCount,
                            options_.keepaliveRequests);
                        scannerEntry.touch();
                        break;
                    }
                    response = co_await routes.dispatch(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        baseRouteServices);
                    connectionPlan = http1FinalizeResponseConnection(
                        response, parsed.connectionPlan.requireClose());
                    break;
                }

                const auto& route = resolved->route();
                const auto& endpoint = route.endpoint();
                const auto maxRequestBodyBytes = requestBodyByteLimit(
                    endpoint.requestBodyMode(),
                    options_.maxStreamBodyBytes,
                    options_.maxBufferedBodyBytes);
                if (contentLengthExceedsLimit(parsed.bodyPlan, maxRequestBodyBytes)) {
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo(413, {}, "request body is too large"),
                        baseRouteServices);
                    connectionPlan = http1FinalizeResponseConnection(
                        response, parsed.connectionPlan.requireClose());
                    break;
                }

                if (endpoint.webSocket() != nullptr) {
                    consumedBytes = parsed.headerBytes;
                    const auto pendingFrames = std::string_view(
                        readBuffer.data() + parsed.headerBytes,
                        usedBytes - parsed.headerBytes);
                    const auto webSocketResult = co_await dispatchHttpWebSocketRoute(
                        stream,
                        memory_,
                        scannerEntry,
                        parsed,
                        *resolved,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        pendingFrames,
                        response,
                        connectionPlan);
                    if (webSocketResult == HttpWebSocketRouteResult::kWriteBufferedResponse) {
                        break;
                    }
                    co_return;
                }

                if (endpoint.responseStream() != nullptr) {
                    consumedBytes = parsed.headerBytes;
                    const auto streamResult = co_await dispatchHttpResponseStreamRoute(
                        stream,
                        memory_,
                        responseHead,
                        scannerEntry,
                        parsed,
                        *resolved,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        response,
                        connectionPlan,
                        requestCount);
                    if (streamResult.finishedSession()) {
                        co_return;
                    }
                    if (streamResult.didDispatchStream()) {
                        responseStreamDispatched = true;
                        bufferAlreadyCompacted = false;
                    }
                    break;
                }
                const auto* bufferedEndpoint = endpoint.buffered();
                if (bufferedEndpoint != nullptr &&
                    bufferedEndpoint->requestBodyMode() ==
                        RequestBodyMode::kStream) {
                    co_await dispatchHttpStreamBodyRoute(
                        stream,
                        memory_,
                        scannerEntry,
                        parsed,
                        routeResolution,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        readBuffer,
                        usedBytes,
                        response,
                        connectionPlan,
                        requestCount,
                        consumedBytes,
                        bufferAlreadyCompacted);
                    break;
                }

                co_await dispatchHttpBufferedBodyRoute(
                    stream,
                    memory_,
                    scannerEntry,
                    parsed,
                    routeResolution,
                    routes,
                    requestMemory,
                    baseRouteServices,
                    options_,
                    readBuffer,
                    usedBytes,
                    response,
                    connectionPlan,
                    requestCount,
                    consumedBytes,
                    bufferAlreadyCompacted);
                break;
            }

            if (parsed.failed()) {
                const auto error = parsed.error;
                if constexpr (kPlainTcp) {
                    if (!options_.autoHttps.enabled &&
                        shouldDropInvalidCleartextHttp1Input(bufferView, error)) {
                        co_return;
                    }
                }
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo(httpParseErrorStatus(error), {}, httpParseErrorMessage(error)),
                    baseRouteServices);
                connectionPlan = http1FinalizeResponseConnection(
                    response, parsed.connectionPlan.requireClose());
                break;
            }

            headerSearchOffset = usedBytes > 3 ? usedBytes - 3 : 0;

            scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
            growReadBuffer(readBuffer, usedBytes, parsed);
            if (usedBytes == readBuffer.size()) {
                constexpr auto error = HttpParseError::kHeaderTooLarge;
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo(httpParseErrorStatus(error), {}, httpParseErrorMessage(error)),
                    baseRouteServices);
                connectionPlan = http1FinalizeResponseConnection(
                    response, parsed.connectionPlan.requireClose());
                break;
            }

            auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
                [&stream, &readBuffer, usedBytes](auto handler) mutable {
                    stream.async_read_some(
                        asio::buffer(readBuffer.data() + usedBytes, readBuffer.size() - usedBytes),
                        std::move(handler));
                });
            if (ec) {
                co_return;
            }

            usedBytes += bytesRead;
            scannerEntry.touch();
        }

        if (!responseStreamDispatched) {
            std::error_code ec;
            scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
            const auto responsePreparation = prepareBufferedHttpResponse(
                parsed.request,
                parsed.responseCoding,
                response,
                options_,
                compressionScratch);
            const auto responsePlan = http1BufferedResponsePlan(
                responsePreparation.writePlan(),
                connectionPlan);
            co_await writeResponse(
                stream,
                memory_,
                &responseHead,
                &fileChunk,
                response,
                responsePlan,
                ec);
            if (responsePreparation.bodyBorrowsCompressionScratch()) {
                clearPmrStringRetainingSmall(compressionScratch, kCompressionScratchRetainedBytes);
            }
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            recordHttpAccess(
                options_.accessLog, parsed.request, remoteAddress,
                response.status(), requestStart, false);
            if (ec ||
                connectionPlan.disposition() == Http1ConnectionDisposition::kClose ||
                !started_.load(std::memory_order_relaxed)) {
                co_return;
            }
        } else {
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            recordHttpAccess(
                options_.accessLog, parsed.request, remoteAddress,
                response.status(), requestStart, false);
            if (!started_.load(std::memory_order_relaxed)) {
                co_return;
            }
        }

        if (!bufferAlreadyCompacted) {
            compactConnectionReadBuffer(readBuffer, usedBytes, consumedBytes);
        }
        trimReadBufferStorage(readBuffer, usedBytes);
    }
}
