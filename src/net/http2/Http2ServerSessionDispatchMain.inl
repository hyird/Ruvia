template <typename Stream>
Task<void> Http2ServerSession<Stream>::dispatchStream(Http2StreamState& stream) {
    stream.dispatchStarted = true;
    const auto requestStart = std::chrono::steady_clock::now();
    std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
    std::optional<RequestMemory> requestMemoryStorage;
    auto& requestMemory = emplaceRequestMemory(
        requestMemoryStorage,
        memory_,
        std::span<std::byte>(arenaBlock.data(), arenaBlock.size()));

    HttpRequest request;
    if (!Http2RequestBuilder::build(stream, request, requestMemory.resource())) {
        HttpResponse response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 400, .message = "invalid http2 request headers"},
            false,
            routeServices());
        co_await writeResponse(stream, response);
        co_return;
    }
    HttpRequestAccess::setTransport(
        request,
        remoteAddress_,
        clientCertificate_,
        !std::is_same_v<Stream, asio::ip::tcp::socket>);
    if (rateLimiter_ != nullptr && rateLimiter_->enabled()) {
        bool rateAllowed = true;
#ifdef RUVIA_ENABLE_REDIS
        if (redis_ != nullptr && !options_.rateLimit.redisAlias.empty()) {
            std::pmr::string rateKey(requestMemory.resource());
            rateKey.append("rl:");
            rateKey.append(remoteAddress_.data(), remoteAddress_.size());
            rateAllowed = co_await redisRateLimitAllow(
                redis_->get(options_.rateLimit.redisAlias, requestMemory.resource()),
                rateKey,
                options_.rateLimit.maxRequests,
                options_.rateLimit.window.count());
        } else
#endif
        {
            rateAllowed = rateLimiter_->allow(remoteAddress_);
        }
        if (!rateAllowed) {
            auto response = co_await routes_.handleError(
                request,
                requestMemory,
                HttpErrorInfo{.statusCode = 429, .message = "rate limit exceeded"},
                false,
                routeServices());
            setRetryAfterSeconds(response, options_.rateLimit.window);
            co_await writeResponse(stream, response);
            recordHttpAccess(
                options_.accessLog, request, remoteAddress_,
                response.statusCode(), requestStart, true);
            co_return;
        }
    }
    const auto& resolution = stream.routeResolution;
    const auto maxBody = resolution.bodyMode == RequestBodyMode::kStream
        ? options_.maxStreamBodyBytes
        : options_.maxBufferedBodyBytes;
    if (maxBody != 0 && stream.body.size() > maxBody) {
        auto response = co_await routes_.handleError(
            request,
            requestMemory,
            HttpErrorInfo{.statusCode = 413, .message = "request body is too large"},
            false,
            routeServices());
        co_await writeResponse(stream, response);
        co_return;
    }

    std::optional<Http2RequestBodyReader<Http2ServerSession>> streamReaderStorage;
    std::optional<BodyReader> bodyReaderStorage;
    if (resolution.bodyMode == RequestBodyMode::kStream) {
        streamReaderStorage.emplace(*this, stream.id);
        bodyReaderStorage.emplace(
            &*streamReaderStorage,
            &Http2RequestBodyReader<Http2ServerSession>::readThunk);
    }
    auto* bodyReader = bodyReaderStorage ? &*bodyReaderStorage : nullptr;

    HttpResponse response(requestMemory.resource());
    const bool foundRoute = resolution.found() && resolution.route != nullptr;
    if (foundRoute && resolution.route->responseMode == ResponseBodyMode::kWebSocket) {
        if (co_await dispatchHttp2WebSocketRoute(stream, request, resolution, requestMemory, response)) {
            co_return;
        }
    } else if (foundRoute && resolution.route->responseMode != ResponseBodyMode::kBuffered) {
        if (co_await dispatchHttp2ResponseStreamRoute(
                stream,
                request,
                resolution,
                requestMemory,
                bodyReader,
                response)) {
            // Streamed/committed on the wire; the buffered tail below is skipped,
            // so log the completed streamed response here (status 200).
            recordHttpAccess(
                options_.accessLog, request, remoteAddress_,
                response.statusCode(), requestStart, true);
            co_return;
        }
    } else {
        response = co_await dispatchHttp2BufferedRoute(stream, request, resolution, requestMemory, bodyReader);
    }

    if (stream.reset) {
        co_return;
    }
    const auto responsePreparation = prepareBufferedHttpResponse(
        request,
        response,
        options_,
        stream.body);
    co_await writeResponse(stream, response, responsePreparation.skipBody);
    if (responsePreparation.bodyBorrowsCompressionScratch) {
        stream.body.clear();
    }
    recordHttpAccess(
        options_.accessLog, request, remoteAddress_,
        response.statusCode(), requestStart, true);
}
