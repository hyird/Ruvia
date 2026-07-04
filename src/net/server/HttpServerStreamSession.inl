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
    const ContextServices baseRouteServices(&databases_, &redis_, &httpClients_, rateLimiter_);
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
        auto& routeMatch = workSet->routeMatch;
        auto& routeResolution = workSet->routeResolution;

        std::optional<RequestMemory> requestMemoryStorage;
        auto& requestMemory = emplaceRequestMemory(
            requestMemoryStorage,
            memory_,
            std::span<std::byte>(workSet->arenaBlock, sizeof(workSet->arenaBlock)));
        HttpResponse response(requestMemory.resource());
        bool keepAlive = false;
        bool closeAfterWrite = false;
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
                        httpClients_,
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
            parser.parseHeaders(bufferView, parsed, headerSearchOffset);
            HttpRequestAccess::setResource(parsed.request, requestMemory.resource());
            if (parsed.status == HttpParseStatus::kComplete) {
                HttpRequestAccess::setTransport(
                    parsed.request,
                    remoteAddress,
                    clientCertificate,
                    !std::is_same_v<Stream, TcpSocket>);
                // Reset phase so headerTimeout stops counting against dispatch
                // time. Body readers will set kReadingBody on their own; the
                // streaming/websocket paths set their own phases below; the
                // buffered write path sets kWriting before responding. Until
                // one of those transitions, idleTimeout governs as the
                // deadman switch for hung handlers.
                scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
                if (options_.autoHttps.enabled) {
                    consumedBytes = parsed.headerBytes;
                    if (requestKnownHeader(parsed.request, RequestKnownHeader::kHost).empty()) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo(400, {}, "missing Host header"),
                            true,
                            baseRouteServices);
                        markConnectionClose(response);
                    } else {
                        response = makeAutoHttpsRedirectResponse(
                            parsed.request,
                            requestMemory,
                            options_.autoHttps.httpsPort);
                    }
                    keepAlive = false;
                    closeAfterWrite = true;
                    scannerEntry.touch();
                    break;
                }
                if constexpr (kPlainTcp) {
                    if (isHttp2UpgradeAttempt(parsed)) {
                        consumedBytes = parsed.headerBytes;
                        const auto upgradeResult = co_await dispatchHttp2UpgradeRoute(
                            stream,
                            socket,
                            memory_,
                            scannerEntry,
                            routes_,
                            databases_,
                            redis_,
                            httpClients_,
                            options_,
                            remoteAddress,
                            rateLimiter_,
                            parsed,
                            readBuffer,
                            usedBytes,
                            requestMemory,
                            baseRouteServices,
                            response,
                            closeAfterWrite,
                            &started_);
                        if (upgradeResult == Http2UpgradeRouteResult::kWriteBufferedResponse) {
                            break;
                        }
                        co_return;
                    }
                }
                routeResolution = routes.resolve(parsed.request, routeMatch);
                const auto appRateLimit = rateLimitRequestAllowed(rateLimiter_, remoteAddress);
                if (!appRateLimit.allowed) {
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo(429, {}, "rate limit exceeded"),
                        true,
                        baseRouteServices);
                    setRetryAfterSeconds(response, std::chrono::milliseconds(appRateLimit.resetAfterMs));
                    markConnectionCloseAfterWrite(response, closeAfterWrite);
                    break;
                }
                if (!routeResolution.found()) {
                    consumedBytes = parsed.headerBytes;
                    if (contentLengthExceedsLimit(parsed.contentLength, options_.maxBufferedBodyBytes)) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo(413, {}, "request body is too large"),
                            true,
                            baseRouteServices);
                        markConnectionCloseAfterWrite(response, closeAfterWrite);
                        break;
                    }
                    if (auto documentResponse = tryDocumentRootResponse(parsed.request, requestMemory)) {
                        response = std::move(*documentResponse);
                        keepAlive = shouldKeepAlive(parsed) &&
                            parsed.contentLength == 0 &&
                            !parsed.chunked;
                        finalizeBufferedRouteResponse(
                            response,
                            keepAlive,
                            requestCount,
                            options_.maxRequestsPerConnection);
                        scannerEntry.touch();
                        break;
                    }
                    response = co_await routes.dispatch(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        baseRouteServices);
                    markConnectionCloseAfterWrite(response, closeAfterWrite);
                    break;
                }

                const auto& route = routeResolution.route();
                const auto maxRequestBodyBytes = requestBodyByteLimit(
                    route.bodyMode(), options_.maxStreamBodyBytes, options_.maxBufferedBodyBytes);
                if (contentLengthExceedsLimit(parsed.contentLength, maxRequestBodyBytes)) {
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo(413, {}, "request body is too large"),
                        true,
                        baseRouteServices);
                    markConnectionCloseAfterWrite(response, closeAfterWrite);
                    break;
                }

                if (route.isWebSocketResponse()) {
                    consumedBytes = parsed.headerBytes;
                    const auto pendingFrames = std::string_view(
                        readBuffer.data() + parsed.headerBytes,
                        usedBytes - parsed.headerBytes);
                    const auto webSocketResult = co_await dispatchHttpWebSocketRoute(
                        stream,
                        memory_,
                        scannerEntry,
                        parsed,
                        routeResolution,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        pendingFrames,
                        response,
                        closeAfterWrite);
                    if (webSocketResult == HttpWebSocketRouteResult::kWriteBufferedResponse) {
                        break;
                    }
                    co_return;
                }

                if (route.isDynamicResponse()) {
                    const auto dynamicResult = co_await dispatchHttpDynamicRoute(
                        stream,
                        memory_,
                        responseHead,
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
                        keepAlive,
                        requestCount,
                        consumedBytes,
                        bufferAlreadyCompacted);
                    if (dynamicResult.finishedSession()) {
                        co_return;
                    }
                    // kStreamDispatched already restored the pipeline and set
                    // bufferAlreadyCompacted; kWriteBufferedResponse falls through
                    // to the buffered write path below.
                    if (dynamicResult.didDispatchStream()) {
                        responseStreamDispatched = true;
                    }
                    break;
                }

                if (route.usesResponseStream()) {
                    consumedBytes = parsed.headerBytes;
                    const auto streamResult = co_await dispatchHttpResponseStreamRoute(
                        stream,
                        memory_,
                        responseHead,
                        scannerEntry,
                        parsed,
                        routeResolution,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        response,
                        keepAlive,
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
                if (route.usesStreamRequestBody()) {
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
                        keepAlive,
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
                    keepAlive,
                    requestCount,
                    consumedBytes,
                    bufferAlreadyCompacted);
                break;
            }

            if (parsed.status == HttpParseStatus::kError) {
                const auto error = parsed.error;
                if constexpr (kPlainTcp) {
                    if (!options_.autoHttps.enabled &&
                        http2ShouldDropInvalidCleartextPreface(bufferView, error)) {
                        co_return;
                    }
                }
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo(httpParseErrorStatus(error), {}, httpParseErrorMessage(error)),
                    true,
                    baseRouteServices);
                closeAfterWrite = true;
                break;
            }

            headerSearchOffset = usedBytes > 3 ? usedBytes - 3 : 0;

            scannerEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
            growReadBuffer(readBuffer, usedBytes, parsed);
            if (usedBytes == readBuffer.size()) {
                constexpr auto error = HttpParseError::kHeaderTooLarge;
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo(httpParseErrorStatus(error), {}, httpParseErrorMessage(error)),
                    true,
                    baseRouteServices);
                closeAfterWrite = true;
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
            co_await writeResponse(
                stream,
                memory_,
                &responseHead,
                &fileChunk,
                response,
                responsePreparation.skipBody,
                ec);
            if (responsePreparation.bodyBorrowsCompressionScratch) {
                clearPmrStringRetainingSmall(compressionScratch, kCompressionScratchRetainedBytes);
            }
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            recordHttpAccess(
                options_.accessLog, parsed.request, remoteAddress,
                response.status(), requestStart, false);
            if (ec || closeAfterWrite || !keepAlive || !started_.load(std::memory_order_relaxed)) {
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
