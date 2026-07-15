template <typename Stream>
Task<void> HttpServer::handleStreamSession(
    Stream& stream,
    TcpSocket& socket,
    ContextServices baseRouteServices) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, the keep-alive request sequence, the remote address, and the count
    // of buffered bytes. The heavy per-request working set (read buffer, request arena,
    // parse result, response head, file chunk) is borrowed from a per-worker
    // pool only while the connection is actively serving and returned the moment
    // it goes idle, so an idle keep-alive connection holds none of it.
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(&connectionScanner_, scannerEntry, socket);
    const auto& routes = routes_;
    const auto remoteAddress =
        baseRouteServices.connInfo().remote().address();
    Http1RequestSequence requestSequence(options_.keepaliveRequests);
    std::size_t usedBytes = 0;
    ConnectionWorkSet* workSet = nullptr;
    WorkSetReturn workSetReturn(workSetPool_, workSet);
    // Connection-resident landing pad for the first bytes of a request that
    // arrives while the connection holds no work set (see the idle wait below).
    std::array<char, kIdleResidentReadBytes> idleReadBuffer;
    std::size_t idleReadBytes = 0;

    constexpr bool kPlainTcp = std::is_same_v<std::remove_cvref_t<Stream>, TcpSocket>;
    for (;;) {
        scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);

        // Borrow-on-use / return-on-idle for the whole work set: when the
        // connection has no buffered bytes, return the work set to the
        // per-worker pool and read the next request's first bytes into the
        // small connection-resident buffer instead, so an idle keep-alive
        // connection occupies no work set (memory scales with in-flight
        // requests, not total connections). Reading directly -- rather than a
        // bufferless readiness wait -- costs no extra reactor pass when data
        // is already queued. A pipelined burst (usedBytes > 0) keeps its work
        // set and skips this. Plain TCP only: a TLS engine may buffer a
        // decrypted record that raw socket readiness cannot see, so an idle
        // wait there could stall; TLS holds across the connection.
        if constexpr (kPlainTcp) {
            if (plainTcpShouldWaitForNextRequest(usedBytes)) {
                releaseIdleWorkSet(workSetPool_, workSet);
                scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
                auto [idleEc, idleBytes] = co_await asyncResult<std::size_t>(
                    [&socket, &idleReadBuffer](auto handler) mutable {
                        socket.async_read_some(
                            asio::buffer(idleReadBuffer.data(), idleReadBuffer.size()),
                            std::move(handler));
                    });
                if (idleEc || !workerRunning_) {
                    co_return;
                }
                idleReadBytes = idleBytes;
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
        auto& routeResolution = workSet->routeResolution;

        if constexpr (kPlainTcp) {
            if (idleReadBytes > 0) {
                static_assert(kIdleResidentReadBytes <= kInitialReadBufferBytes);
                std::memcpy(readBuffer.data(), idleReadBuffer.data(), idleReadBytes);
                usedBytes = idleReadBytes;
                idleReadBytes = 0;
                scannerEntry.touch();
            }
        }

        std::optional<RequestMemory> requestMemoryStorage;
        auto& requestMemory = emplaceRequestMemory(
            requestMemoryStorage,
            memory_,
            std::span<std::byte>(workSet->arenaBlock, sizeof(workSet->arenaBlock)));
        HttpResponse response(requestMemory.resource());
        std::optional<Http1SessionRequestCompletion> requestCompletion;
        // Rejections that close the connection funnel through one co_await
        // site after the read loop: every co_await expression in a coroutine
        // reserves its own frame slots for the call's temporaries (GCC does
        // not overlap them), so inlining handleError at each rejection site
        // costs ~660 resident bytes per site in every connection's frame.
        std::optional<HttpErrorInfo> closingError;
        std::optional<std::chrono::milliseconds> closingRetryAfter;
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
                        options_,
                        scannerEntry,
                        baseRouteServices,
                        readBuffer,
                        usedBytes,
                        workerRunning_);
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
            if (const auto* requestHead = parsed.headReady()) {
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
                    closingError = HttpErrorInfo(417, {}, "unsupported Expect header");
                    break;
                }
                if (options_.autoHttps.enabled) {
                    if (requestKnownHeader(parsed.request, RequestKnownHeader::kHost).empty()) {
                        closingError = HttpErrorInfo(400, {}, "missing Host header");
                        break;
                    }
                    response = makeAutoHttpsRedirectResponse(
                        parsed.request,
                        requestMemory,
                        options_.autoHttps.httpsPort);
                    const auto connectionPlan = requireHttp1FinalResponseCommit(
                        response,
                        parsed.connectionPlan.requireClose());
                    requestCompletion.emplace(
                        Http1SessionRequestCompletion::makeBufferedClosing(
                            connectionPlan));
                    scannerEntry.touch();
                    break;
                }
                routeResolution = routes.resolve(parsed.request);
                const auto appRateLimit = rateLimitRequestAllowed(&rateLimiter_, remoteAddress);
                if (!appRateLimit.allowed) {
                    closingError = HttpErrorInfo(429, {}, "rate limit exceeded");
                    closingRetryAfter.emplace(appRateLimit.resetAfterMs);
                    break;
                }
                const auto* resolved = routeResolution.resolved();
                if (resolved == nullptr) {
                    if (contentLengthExceedsLimit(
                            parsed.bodyPlan,
                            ProtocolByteLimit::limited(
                                options_.maxBufferedBodyBytes))) {
                        closingError = HttpErrorInfo(413, {}, "request body is too large");
                        break;
                    }
                    if (auto documentResponse = tryDocumentRootResponse(parsed.request, requestMemory)) {
                        response = std::move(*documentResponse);
                        auto connectionPlan = http1ApplyRequestBodyConsumption(
                            parsed.connectionPlan,
                            parsed.bodyPlan.requiresConsumption()
                                ? Http1RequestBodyConsumption::kIncomplete
                                : Http1RequestBodyConsumption::kComplete);
                        connectionPlan = finalizeBufferedRouteResponse(
                            response,
                            connectionPlan,
                            requestSequence);
                        requestCompletion.emplace(
                            Http1SessionRequestCompletion::makeBufferedUnrestored(
                                connectionPlan,
                                requestHead->headerBytes()));
                        scannerEntry.touch();
                        break;
                    }
                    response = co_await routes.dispatch(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        baseRouteServices);
                    const auto connectionPlan = requireHttp1FinalResponseCommit(
                        response, parsed.connectionPlan.requireClose());
                    requestCompletion.emplace(
                        Http1SessionRequestCompletion::makeBufferedClosing(
                            connectionPlan));
                    break;
                }

                const auto& route = resolved->route();
                const auto& endpoint = route.endpoint();
                const auto maxRequestBodyBytes = requestBodyByteLimit(
                    endpoint.requestBodyMode(),
                    options_.maxStreamBodyBytes,
                    options_.maxBufferedBodyBytes);
                if (contentLengthExceedsLimit(parsed.bodyPlan, maxRequestBodyBytes)) {
                    closingError = HttpErrorInfo(413, {}, "request body is too large");
                    break;
                }

                if (endpoint.webSocket() != nullptr) {
                    const auto pendingFrames = std::string_view(
                        readBuffer.data() + requestHead->headerBytes(),
                        usedBytes - requestHead->headerBytes());
                    auto webSocketCompletion = co_await dispatchHttpWebSocketRoute(
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
                        response);
                    if (!webSocketCompletion.has_value()) {
                        co_return;
                    }
                    requestCompletion.emplace(
                        std::move(*webSocketCompletion));
                    break;
                }

                if (endpoint.responseStream() != nullptr) {
                    requestCompletion.emplace(
                        co_await dispatchHttpResponseStreamRoute(
                        stream,
                        memory_,
                        responseHead,
                        scannerEntry,
                        parsed,
                        *requestHead,
                        *resolved,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        response,
                        requestSequence));
                    break;
                }
                const auto* bufferedEndpoint = endpoint.buffered();
                if (bufferedEndpoint != nullptr &&
                    bufferedEndpoint->requestBodyMode() ==
                        RequestBodyMode::kStream) {
                    requestCompletion.emplace(
                        co_await dispatchHttpStreamBodyRoute(
                        stream,
                        memory_,
                        scannerEntry,
                        parsed,
                        *requestHead,
                        routeResolution,
                        routes,
                        requestMemory,
                        baseRouteServices,
                        options_,
                        readBuffer,
                        usedBytes,
                        response,
                        requestSequence));
                    break;
                }

                // Buffered-body dispatch, inlined into the session loop: this
                // is the hot path for every plain buffered route, and a
                // dedicated coroutine here would cost one frame allocation
                // per request.
                {
                    const auto bodyAndPipeline = httpBodyAndPipeline(
                        *requestHead,
                        readBuffer,
                        usedBytes);

                    // The body reader/loader setup can throw (e.g. constructing a
                    // transfer-coding decoder for a bad Transfer-Encoding), so it
                    // stays guarded. The dispatch itself never throws:
                    // dispatchBuffered turns any handler or routing failure into
                    // a response, so it sits outside the guard.
                    std::exception_ptr bodySetupException;
                    HttpLazyBufferedBodyRouteState<Stream> bodyState;
                    try {
                        prepareHttpLazyBufferedBodyRoute(
                            bodyState,
                            stream,
                            memory_,
                            requestMemory,
                            bodyAndPipeline,
                            parsed,
                            options_,
                            scannerEntry);
                    } catch (...) {
                        bodySetupException = std::current_exception();
                    }

                    if (bodySetupException != nullptr) {
                        requestCompletion.emplace(co_await completeFailedHttpBodyRoute(
                            scannerEntry,
                            bodySetupException,
                            parsed,
                            routes,
                            requestMemory,
                            baseRouteServices,
                            response));
                        break;
                    }

                    response = co_await routes.dispatchBuffered(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        bodyState.withLoader(baseRouteServices));

                    requestCompletion.emplace(completeSuccessfulHttpBodyRoute(
                        scannerEntry,
                        response,
                        parsed.connectionPlan,
                        requestSequence,
                        bodyState.consumption(),
                        readBuffer,
                        usedBytes,
                        [&bodyState](std::pmr::string& buffer, std::size_t& size) {
                            bodyState.restorePipeline(buffer, size);
                        }));
                    break;
                }
            }

            if (const auto* failure = parsed.failure()) {
                const auto error = failure->error();
                if constexpr (kPlainTcp) {
                    if (!options_.autoHttps.enabled &&
                        shouldDropInvalidCleartextHttp1Input(bufferView, error)) {
                        co_return;
                    }
                }
                closingError = HttpErrorInfo(
                    httpParseErrorStatus(error),
                    {},
                    httpParseErrorMessage(error));
                break;
            }

            headerSearchOffset = usedBytes > 3 ? usedBytes - 3 : 0;

            scannerEntry.setPhase(ConnectionScanner::Phase::kReadingInitial);
            growReadBuffer(readBuffer, usedBytes);
            if (usedBytes == readBuffer.size()) {
                constexpr auto error = HttpParseError::kHeaderTooLarge;
                closingError = HttpErrorInfo(
                    httpParseErrorStatus(error),
                    {},
                    httpParseErrorMessage(error));
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

        // Shared exit for every rejection recorded above: one co_await site
        // keeps one set of call temporaries in the frame instead of one per
        // rejection branch.
        if (closingError) {
            response = co_await routes.handleError(
                parsed.request,
                requestMemory,
                *closingError,
                baseRouteServices);
            if (closingRetryAfter) {
                setRetryAfterSeconds(response, *closingRetryAfter);
            }
            requestCompletion.emplace(
                Http1SessionRequestCompletion::makeBufferedClosing(
                    requireHttp1FinalResponseCommit(
                        response, parsed.connectionPlan.requireClose())));
        }

        if (!requestCompletion) {
            throw std::logic_error(
                "HTTP/1 request dispatch returned no terminal completion");
        }
        const auto connectionPlan = requestCompletion->connectionPlan();
        if (requestCompletion->bufferedResponse() != nullptr) {
            scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
            const auto writePlan = prepareBufferedHttpResponse(
                parsed.request,
                parsed.responseCoding,
                response,
                options_);
            const auto responsePlan = http1BufferedResponsePlan(
                writePlan,
                connectionPlan);
            const auto writeResult = co_await writeResponse(
                stream,
                memory_,
                &responseHead,
                &fileChunk,
                response,
                responsePlan);
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            if (const auto committedStatus = writeResult.committedStatus()) {
                recordHttpAccess(
                    options_.accessLog,
                    parsed.request,
                    remoteAddress,
                    *committedStatus,
                    requestStart);
            }
            if (writeResult.completed() == nullptr) {
                co_return;
            }
        } else if (const auto* committed =
                       requestCompletion->committedStream()) {
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            recordHttpAccess(
                options_.accessLog, parsed.request, remoteAddress,
                committed->status(), requestStart);
        } else {
            throw std::logic_error(
                "HTTP/1 request completion has no wire alternative");
        }

        if (connectionPlan.disposition() ==
                Http1ConnectionDisposition::kClose ||
            !workerRunning_) {
            co_return;
        }
        applyReusableHttp1RequestBufferCompletion(
            requestCompletion->bufferCompletion(),
            readBuffer,
            usedBytes);
        trimReadBufferStorage(readBuffer, usedBytes);
    }
}
