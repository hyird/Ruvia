#pragma once

// Concurrent HTTP/2 server session over the sans-I/O core (ruvia-web).
//
// Drives an Http2Connection with a reader coroutine + a single writer coroutine so that
// HTTP/2 streams are genuinely multiplexed: each admitted request is dispatched on its
// own concurrent handler task, and a slow handler never blocks reading or the other
// streams. All outbound bytes funnel through the one writer (submit* only appends to the
// connection's buffer, so ordering is automatic); the writer sleeps on a signal timer
// when idle and is woken whenever new output is produced.
//
// Framework parity with the coroutine Http2ServerSession (which this replaces): route
// policy resolved at kMessageHead (mirroring resolveStreamRoute), rate limiting,
// request-body size limits, streaming request bodies (BodyReader over the stream's
// body-chunk queue), WebSocket tunnels (RFC 8441), streaming + buffered + file-body
// responses with compression/CORS via prepareBufferedHttpResponse, access logging,
// client certificates, connection-scanner inactivity phases, graceful drain on server
// shutdown, and h2c upgrade seeding (RFC 7540 §3.2).
//
// Lifetime safety: a request/response holds VIEWS into its stream's decoded storage, so
// before spawning a handler the stream is pinned (see Http2Connection::pinStream) -- a
// peer RST then keeps the stream alive+reset rather than freeing it, and the handler
// checks isReset() before submitting. The handler unpins on completion, freeing it.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "HttpParserInternal.h"
#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseFileAccess.h"
#include "http/ContextServices.h"
#include "net/HttpFileOpen.h"
#include "net/RequestBodyLimit.h"
#include "net/RequestMemoryArena.h"
#include "net/body/HttpRequestBodyFacade.h"
#include "net/http2/Http2Connection.h"
#include "net/http2/Http2RequestBuilder.h"
#include "net/http2/Http2SansIoResponseStreamSink.h"
#include "net/http2/Http2SansIoWsTransport.h"
#include "net/http2/Http2WebSocketHandshake.h"
#include "net/server/ConnectionScanner.h"
#include "net/server/HttpBufferedResponse.h"
#include "net/server/HttpFileChunkBuffer.h"
#include "net/server/HttpResponseHeadPolicy.h"
#include "net/server/HttpResponseStreamDispatch.h"
#include "net/server/HttpServerAccessLog.h"
#include "net/server/HttpServerResponseState.h"
#include "net/server/RateLimitDecision.h"
#include "net/ws/HttpWebSocketConnection.h"
#include "net/ws/HttpWebSocketPermessageDeflate.h"
#include "net/ws/HttpWebSocketSession.h"
#include "router/RequestDispatcher.h"
#include "router/RouteResolution.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/http/HttpServerOptions.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

// h2c upgrade seed (RFC 7540 §3.2): the parsed h1 request + its HTTP2-Settings payload
// and body, handed to Http2Connection::beginUpgraded before the frame loop starts.
struct Http2SansIoUpgradeSeed final {
    const HttpServerParseResult* parsed{nullptr};
    std::string_view settingsPayload{};
    std::string_view body{};
};

// Framework wiring for the sans-I/O h2 session. Every field defaults to "absent" so
// tests can drive the session bare; the accept loop passes the full server context.
struct Http2SansIoSessionEnv final {
    DbRegistry* databases{nullptr};
    RedisRegistry* redis{nullptr};
    HttpClientRegistry* httpClients{nullptr};
    RateLimiter* rateLimiter{nullptr};
    const HttpServerOptions* options{nullptr};        // null -> default options
    ConnectionScanner::Entry* scannerEntry{nullptr};  // null -> unlinked local entry
    std::string_view clientCertificate{};
    const std::atomic_bool* serverStarted{nullptr};   // false -> graceful GOAWAY drain
    const Http2SansIoUpgradeSeed* upgrade{nullptr};   // h2c upgrade seeding
};

template <typename Stream>
Task<void> runHttp2SansIoSession(
    Stream& stream,
    const RequestDispatcher& routes,
    WorkerMemory& worker,
    std::string_view remoteAddress,
    Http2SansIoSessionEnv env = {},
    std::string_view initialBytes = {}) {
    auto executor = stream.get_executor();
    static const HttpServerOptions kDefaultOptions{};
    const HttpServerOptions& options = env.options != nullptr ? *env.options : kDefaultOptions;
    ConnectionScanner::Entry localScannerEntry;
    ConnectionScanner::Entry& scannerEntry =
        env.scannerEntry != nullptr ? *env.scannerEntry : localScannerEntry;
    constexpr bool kTlsStream = !std::is_same_v<Stream, asio::ip::tcp::socket>;

    Http2CoreConfig coreConfig;
    coreConfig.maxStreamBodyBytes = options.maxStreamBodyBytes;
    coreConfig.maxBufferedBodyBytes = options.maxBufferedBodyBytes;
    Http2Connection connection(worker.resource(), coreConfig);

    asio::steady_timer writeSignal(executor);
    int inFlight = 0;       // concurrent handlers not yet finished
    bool stopping = false;  // reader has finished (EOF/error/closing)
    bool writeFailed = false;
    bool writerDone = false;  // writer coroutine has fully exited (level-triggered join)

    const auto wakeWriter = [&writeSignal]() noexcept {
        writeSignal.cancel();  // wakes async_wait with operation_aborted
    };

    // One wake signal per stream dispatched with async plumbing (WebSocket tunnel or
    // streaming request body), owned here: created by the reader at admission, erased
    // by the stream's handler task when it finishes.
    std::vector<std::pair<std::uint32_t, std::unique_ptr<Http2SansIoStreamSignal>>> streamSignals;
    const auto findSignal = [&streamSignals](std::uint32_t streamId) noexcept -> Http2SansIoStreamSignal* {
        for (const auto& entry : streamSignals) {
            if (entry.first == streamId) {
                return entry.second.get();
            }
        }
        return nullptr;
    };

    // Single writer: serialize all outbound writes; sleep on writeSignal when idle.
    // The pending bytes are MOVED out before each write (takeOutput): concurrent
    // handlers keep submitting while async_write is in flight, and an append that
    // reallocated the core's buffer would dangle a pendingOutput() view mid-write.
    //
    // CRITICAL: the writer's ONLY exit is `stopping && inFlight == 0`. A write error
    // sets writeFailed but keeps looping (draining and discarding output) until every
    // in-flight handler has finished, because the session's teardown join treats the
    // writer's exit as the handler join too -- exiting early with inFlight > 0 would
    // let the session destroy this frame's state under a still-suspended detached
    // handler (use-after-free). Once writeFailed, no bytes actually reach the socket.
    auto writerLoop = [&]() -> Task<void> {
        std::pmr::string writeScratch(worker.resource());
        for (;;) {
            while (connection.wantsWrite()) {
                connection.takeOutput(writeScratch);  // always drain so the buffer can't grow
                if (writeFailed) {
                    continue;  // socket is dead; discard, but keep serving handlers
                }
                const auto ec = co_await asyncError([&stream, &writeScratch](auto handler) mutable {
                    asio::async_write(
                        stream, asio::buffer(writeScratch.data(), writeScratch.size()),
                        std::move(handler));
                });
                if (ec) {
                    writeFailed = true;
                    continue;
                }
                scannerEntry.touch();
                // Release a peak-sized batch buffer so it (and, via takeOutput's swap,
                // the core's outBuffer_) does not pin the connection's high-water
                // capacity for its whole lifetime (the h1 work-set trims the same way).
                clearPmrStringRetainingSmall(writeScratch, 64 * 1024);
            }
            if (stopping && inFlight == 0) {
                co_return;  // nothing left to write and no more will be produced
            }
            writeSignal.expires_at((asio::steady_timer::time_point::max)());
            co_await asyncError([&writeSignal](auto handler) mutable {
                writeSignal.async_wait(std::move(handler));
            });
        }
    };

    // Wait until the reader reports this stream's window-blocked remainder drained
    // (via takeUnblockedStreams -> signal wake). Loops on hasBlockedSend so a false
    // wake (a request-body chunk, or another stream's unblock) does NOT let the caller
    // read the next body chunk ahead while still blocked -- that would grow the core's
    // pendingSends_ without bound and defeat the backpressure. Returns false (stop) if
    // the stream died or the signal ended (session teardown).
    auto awaitSendWindow = [&](std::uint32_t streamId) -> Task<bool> {
        auto* signal = findSignal(streamId);
        for (;;) {
            auto* live = connection.stream(streamId);
            if (live == nullptr || live->isReset()) {
                co_return false;
            }
            if (!connection.hasBlockedSend(streamId)) {
                co_return true;  // window reopened; the remainder drained
            }
            if (signal == nullptr || signal->ended) {
                co_return false;
            }
            co_await signal->wait();
        }
    };

    // Submit a complete response through the core, mirroring the coroutine
    // writeResponse: HEADERS (auto Content-Length for plain/file bodies), then the
    // body as DATA -- buffered bytes in one submit, a file body read in chunks, or an
    // async stream body pulled chunk by chunk. Chunked bodies pace themselves on the
    // send window via awaitSendWindow so a slow client never balloons the out-buffer.
    auto submitResponse = [&](std::uint32_t streamId, const HttpResponse& response,
                              bool skipBody) -> Task<void> {
        auto* streamState = connection.stream(streamId);
        if (streamState == nullptr || streamState->isReset()) {
            co_return;
        }
        const auto policy = responseWritePolicy(response.status());
        const bool sendBody = policy.bodyAllowed() && !skipBody;
        if (responseHasStreamBody(response)) {
            // A normal route returned a streaming body (e.g. Context::proxy): HEADERS
            // without a content-length, then DATA pulled from the source; a mid-body
            // failure aborts the stream (RFC 9113 §8.1).
            connection.submitStreamingResponseHead(streamId, response, !sendBody);
            wakeWriter();
            if (!sendBody) {
                co_return;
            }
            const auto& body = HttpResponseBodyAccess::stream(response);
            for (;;) {
                auto* live = connection.stream(streamId);
                if (live == nullptr || live->isReset()) {
                    co_return;
                }
                std::string_view chunk;
                bool failed = false;
                try {
                    chunk = co_await body.nextChunk();
                } catch (...) {
                    failed = true;
                }
                if (failed) {
                    connection.submitReset(
                        streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
                    wakeWriter();
                    co_return;
                }
                if (chunk.empty()) {
                    break;
                }
                const auto result = connection.submitData(streamId, chunk, false);
                wakeWriter();
                if (result == Http2SubmitResult::kClosed) {
                    co_return;
                }
                if (result == Http2SubmitResult::kBlocked && !(co_await awaitSendWindow(streamId))) {
                    co_return;
                }
            }
            (void)connection.submitData(streamId, {}, true);
            wakeWriter();
            co_return;
        }
        std::uint64_t contentLength = 0;
        if (policy.bodyAllowed()) {
            contentLength = responseHasFileBody(response)
                ? responseFileBody(response).length
                : responseBodySize(response);
        }
        connection.submitResponseHead(streamId, response, skipBody);
        wakeWriter();
        if (!sendBody || contentLength == 0) {
            co_return;
        }
        if (responseHasFileBody(response)) {
            const auto fileBody = responseFileBody(response);
            auto input = openResponseFileInput(fileBody);
            bool ready = static_cast<bool>(input);
            if (ready) {
                input.seekg(static_cast<std::streamoff>(fileBody.offset), std::ios::beg);
                ready = static_cast<bool>(input);
            }
            if (!ready) {
                // The advertised content-length can no longer be honoured (file gone
                // or shrank): abort the stream rather than under-send (RFC 9113 §8.1.1).
                connection.submitReset(
                    streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
                wakeWriter();
                co_return;
            }
            std::pmr::string fileChunk(worker.allocator<char>());
            ensureFileChunkBuffer(fileChunk);
            std::uint64_t remaining = fileBody.length;
            while (remaining > 0) {
                auto* live = connection.stream(streamId);
                if (live == nullptr || live->isReset()) {
                    co_return;
                }
                const auto next = static_cast<std::size_t>(
                    std::min<std::uint64_t>(fileChunk.size(), remaining));
                input.read(fileChunk.data(), static_cast<std::streamsize>(next));
                const auto readBytes = input.gcount();
                if (readBytes <= 0) {
                    connection.submitReset(
                        streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
                    wakeWriter();
                    co_return;
                }
                remaining -= static_cast<std::uint64_t>(readBytes);
                const auto result = connection.submitData(
                    streamId,
                    std::string_view(fileChunk.data(), static_cast<std::size_t>(readBytes)),
                    remaining == 0);
                wakeWriter();
                if (result == Http2SubmitResult::kClosed) {
                    co_return;
                }
                if (result == Http2SubmitResult::kBlocked && !(co_await awaitSendWindow(streamId))) {
                    co_return;
                }
            }
            co_return;
        }
        // Buffered bytes: a window-blocked remainder drains inside the core.
        (void)connection.submitData(streamId, responseBodyBytes(response), true);
        wakeWriter();
        co_return;
    };

    // Handler body for one admitted stream; a 1:1 port of the coroutine
    // dispatchStream. Early co_returns are safe -- dispatchOne (below) owns cleanup.
    auto dispatchOneInner = [&](std::uint32_t streamId) -> Task<void> {
        const auto requestStart = std::chrono::steady_clock::now();
        // Seed the request arena from a block carried on THIS coroutine frame (already
        // heap-allocated by co_spawn), so the common request does zero extra heap work
        // -- restoring the coroutine session's zero-alloc invariant that the retrofit
        // lost by constructing RequestMemory(worker) (a separate >=4KB alloc/free pair).
        std::array<std::byte, kRequestArenaStackBytes> arenaBlock;
        std::optional<RequestMemory> requestMemoryStorage;
        RequestMemory& requestMemory = emplaceRequestMemory(
            requestMemoryStorage, worker,
            std::span<std::byte>(arenaBlock.data(), arenaBlock.size()));
        auto* streamState = connection.stream(streamId);
        if (streamState == nullptr) {
            co_return;
        }
        const ContextServices baseServices(
            env.databases, env.redis, env.httpClients, env.rateLimiter);

        HttpRequest request = HttpRequestAccess::make();
        if (!Http2RequestBuilder::build(*streamState, request, requestMemory.resource())) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(400, {}, "invalid http2 request headers"),
                false, baseServices);
            co_await submitResponse(streamId, response, false);
            co_return;
        }
        HttpRequestAccess::setTransport(request, remoteAddress, env.clientCertificate, kTlsStream);
        const auto& resolution = streamState->routeResolution();

        const auto appRateLimit = rateLimitRequestAllowed(env.rateLimiter, remoteAddress);
        if (!appRateLimit.allowed) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(429, {}, "rate limit exceeded"),
                false, baseServices);
            setRetryAfterSeconds(response, std::chrono::milliseconds(appRateLimit.resetAfterMs));
            co_await submitResponse(streamId, response, false);
            recordHttpAccess(
                options.accessLog, request, remoteAddress, response.status(), requestStart, true);
            co_return;
        }
        const auto maxBody = requestBodyByteLimit(
            streamState->bodyMode(), options.maxStreamBodyBytes, options.maxBufferedBodyBytes);
        if (maxBody != 0 && streamState->requestBodySize() > maxBody) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(413, {}, "request body is too large"),
                false, baseServices);
            co_await submitResponse(streamId, response, false);
            co_return;
        }

        std::optional<Http2SansIoRequestBodyReader> streamReaderStorage;
        std::optional<BodyReader> bodyReaderStorage;
        if (streamState->usesStreamRequestBody() && !streamState->webSocketTunnel()) {
            if (!streamState->requestBodyEmpty()) {
                // An h2c-upgrade seeded body (or END_STREAM-on-HEADERS request) was
                // buffered before dispatch; hand it to the reader as the first chunk.
                streamState->enqueueBufferedRequestBodyChunk();
            }
            streamReaderStorage.emplace(connection, streamId, findSignal(streamId));
            emplaceBodyReaderFacade(bodyReaderStorage, *streamReaderStorage);
        }
        auto dispatchServices = baseServices;
        if (bodyReaderStorage) {
            dispatchServices = dispatchServices.withBodyReader(*bodyReaderStorage);
        }

        HttpResponse response(requestMemory.resource());
        if (resolution.found() && resolution.isWebSocketResponse()) {
            if (http2IsValidWebSocketRequest(*streamState, request)) {
                // RFC 7692 permessage-deflate over h2 (parity with the h1 handshake).
                const auto deflate = webSocketNegotiatePermessageDeflate(request);
                connection.submitWebSocketHandshake(
                    streamId,
                    http2ChooseWebSocketSubprotocol(request, resolution.webSocketSubprotocols()),
                    webSocketDeflateResponseExtensions(deflate));
                wakeWriter();  // flush the 200 before the first tunnel read suspends
                auto* signal = findSignal(streamId);
                if (signal == nullptr) {
                    co_return;  // ws streams are admitted with a signal; defensive
                }
                using WsTransport = Http2SansIoWsTransport<decltype(executor)>;
                WebSocketConnection<WsTransport> webSocketConnection(
                    WsTransport(connection, streamId, *signal, writeSignal, executor),
                    scannerEntry,
                    resolution.webSocketHeartbeat(),
                    options.maxWebSocketMessageBytes,
                    requestMemory.resource(),
                    /*initialBytes=*/{},
                    deflate.enabled);
                co_await runWebSocketSession(
                    webSocketConnection, scannerEntry, routes, request, resolution,
                    requestMemory, baseServices);
                co_return;  // tunnel handled on the wire; no buffered tail (parity)
            }
            response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(400, {}, "invalid http2 websocket request"),
                false, baseServices);
        } else if (resolution.found() && resolution.usesResponseStream()) {
            // Streaming route (Context::proxy / SSE): drive the shared streaming
            // dispatch through a sans-I/O sink that submits chunks via the core.
            Http2SansIoResponseStreamSink<decltype(executor)> sink(
                connection, streamId, resolution.responseMode(), requestMemory.resource(),
                executor, &writeSignal, findSignal(streamId));
            auto result = co_await dispatchResponseStreamWith(
                sink, routes, request, resolution, requestMemory, dispatchServices,
                /*closeConnectionOnError=*/false,
                [&connection, streamId]() noexcept {
                    auto* s = connection.stream(streamId);
                    return s == nullptr || s->isReset();
                });
            if (result.abortedAfterCommit()) {
                connection.submitReset(
                    streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
                wakeWriter();
                recordHttpAccess(
                    options.accessLog, request, remoteAddress, response.status(), requestStart, true);
                co_return;
            }
            if (result.streamed() || result.abortedByPeer()) {
                // Streamed on the wire; log the completed streamed response (status 200).
                recordHttpAccess(
                    options.accessLog, request, remoteAddress, response.status(), requestStart, true);
                co_return;
            }
            if (result.hasBufferedResponse()) {
                response = result.takeResponse();
            }
        } else {
            response = co_await routes.dispatchBuffered(
                request, resolution, requestMemory, false, dispatchServices);
        }

        auto* live = connection.stream(streamId);
        if (live == nullptr || live->isReset()) {
            co_return;
        }
        const auto preparation = prepareBufferedHttpResponse(
            request, response, options, live->responseCompressionScratch());
        co_await submitResponse(streamId, response, preparation.skipBody);
        if (preparation.bodyBorrowsCompressionScratch) {
            live->clearRequestBody();
        }
        recordHttpAccess(
            options.accessLog, request, remoteAddress, response.status(), requestStart, true);
    };

    // One concurrent handler: run the dispatch body, then release the stream's
    // resources (signal, pin) and wake the writer so the session can make progress.
    auto dispatchOne = [&](std::uint32_t streamId) -> Task<void> {
        ++inFlight;
        try {
            co_await dispatchOneInner(streamId);
        } catch (...) {
            // Last-resort: a dispatch failure must not leak the pin/inFlight count.
            auto* live = connection.stream(streamId);
            if (live != nullptr && !live->isReset()) {
                connection.submitReset(
                    streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
            }
        }
        std::erase_if(streamSignals, [streamId](const auto& entry) {
            return entry.first == streamId;
        });
        connection.unpinStream(streamId);
        --inFlight;
        wakeWriter();
        co_return;
    };

    // Owner-side route policy (1:1 port of the coroutine resolveStreamRoute), run at
    // kMessageHead so body-mode/tunnel decisions land BEFORE the next feed.
    const auto resolveStreamRoute = [&routes](Http2StreamState& streamState) noexcept {
        const auto method = Http2RequestBuilder::requestMethod(streamState);
        const auto path = Http2RequestBuilder::requestPath(streamState);
        if (method == HttpMethod::kUnknown || path.empty()) {
            streamState.resetRoutingToBuffered();
            return;
        }
        streamState.setRouteResolution(routes.resolve(method, path, streamState.routeMatch()));
        const auto& resolution = streamState.routeResolution();
        if (!resolution.found()) {
            streamState.setBodyMode(RequestBodyMode::kBuffered);
            return;
        }
        streamState.setBodyMode(resolution.bodyMode());
        if (streamState.extendedConnectWebSocket() && resolution.isWebSocketResponse()) {
            streamState.markWebSocketTunnel();
            streamState.setBodyMode(RequestBodyMode::kStream);
        }
    };

    // Admit a stream for dispatch: EVERY dispatched stream gets a signal, so response
    // send-window pacing (awaitSendWindow / the streaming sink) can be woken by
    // takeUnblockedStreams no matter the body kind -- a plain route returning a large
    // file or a stream body blocks on the send window exactly like a streaming route,
    // and without a signal its blocked submit could never be woken (truncation + hang).
    const auto admitStream = [&](std::uint32_t streamId) {
        streamSignals.emplace_back(
            streamId, std::make_unique<Http2SansIoStreamSignal>(executor));
        connection.pinStream(streamId);
        asio::co_spawn(
            executor, taskAsAwaitable(dispatchOne(streamId)), asio::detached);
    };

    // Drain the core's event queue, admitting streams and routing body bytes.
    const auto drainEvents = [&]() {
        for (;;) {
            const auto event = connection.nextEvent();
            if (event.kind == Http2Event::Kind::kNone) {
                break;
            }
            auto* streamState = connection.stream(event.streamId);
            if (streamState == nullptr) {
                continue;
            }
            if (event.kind == Http2Event::Kind::kMessageHead) {
                resolveStreamRoute(*streamState);
                const bool wsTunnel = streamState->webSocketTunnel();
                const bool streamingBody = !wsTunnel &&
                    streamState->usesStreamRequestBody() && !streamState->bodyEnded();
                if (wsTunnel || streamingBody) {
                    // Dispatch NOW; body bytes stream through the stream's chunk queue
                    // while the handler runs (mirrors queueInitialStreamIfReady).
                    admitStream(event.streamId);
                }
            } else if (event.kind == Http2Event::Kind::kMessageBodyChunk) {
                if (streamState->webSocketTunnel() || streamState->usesStreamRequestBody()) {
                    streamState->enqueueBodyChunk(event.bytes);
                    if (auto* signal = findSignal(event.streamId)) {
                        signal->wake();
                    }
                } else {
                    streamState->appendRequestBody(event.bytes);
                }
            } else if (event.kind == Http2Event::Kind::kMessageEnd) {
                if (auto* signal = findSignal(event.streamId)) {
                    signal->wake();  // bodyEnded was marked by the core before this event
                } else {
                    admitStream(event.streamId);  // buffered request: dispatch at end
                }
            } else if (event.kind == Http2Event::Kind::kStreamClosed) {
                if (auto* signal = findSignal(event.streamId)) {
                    signal->wake();  // stream is reset; blocked readers/writers see it
                }
            }
        }
        // Send-window reopenings drained deferred bodies inside the core; wake the
        // paced response writers so they pull their next chunk.
        for (const auto streamId : connection.takeUnblockedStreams()) {
            if (auto* signal = findSignal(streamId)) {
                signal->wake();
            }
        }
    };

    // --- startup ----------------------------------------------------------------
    if (env.upgrade != nullptr) {
        (void)connection.beginUpgraded(
            *env.upgrade->parsed, env.upgrade->settingsPayload, env.upgrade->body);
    } else {
        connection.expectClientPreface();
        connection.queueLocalSettings();
    }

    // Spawn the writer; it runs concurrently with the reader below. The join below is
    // LEVEL-triggered on writerDone, not on the timer cancel alone: steady_timer::cancel
    // only aborts an already-registered wait, so if the writer finishes before the
    // reader reaches the join (common on an abrupt peer RST, where the write error
    // surfaces first), a cancel-only latch would be a no-op and the join would sleep
    // until time_point::max() forever. The bool makes a late join return immediately.
    asio::steady_timer writerFinished(executor);
    writerFinished.expires_at((asio::steady_timer::time_point::max)());
    asio::co_spawn(
        executor, taskAsAwaitable(writerLoop()),
        [&writerFinished, &writerDone](std::exception_ptr) noexcept {
            writerDone = true;
            writerFinished.cancel();
        });

    // Drain the h2c seeded request BEFORE feeding any initial bytes: feed() resets the
    // event queue at entry (its per-feed view contract), which would wipe the seed's
    // kMessageHead/kMessageEnd when the client pipelined its preface after the
    // upgrade request. Seed events carry no input views, so draining first is safe.
    drainEvents();
    if (!connection.closing() && !initialBytes.empty()) {
        (void)connection.feed(initialBytes);
        drainEvents();
    }
    wakeWriter();

    // Reader loop: feed inbound bytes, then act on the drained events.
    if (!connection.closing()) {
        std::array<char, 16384> readBuffer;
        for (;;) {
            // Pick the inactivity phase: mid-header-block -> the tight header timeout;
            // nothing in flight (no admitted handler, no signalled tunnel/body stream)
            // -> kIdle so a keep-alive connection between requests is governed by the
            // keepalive timeout, NOT client_body_timeout; otherwise a body/response is
            // in progress -> kReadingBody. (WS tunnels carry their own heartbeat.)
            scannerEntry.setPhase(
                connection.headerBlockInProgress()
                    ? ConnectionScanner::Phase::kReadingHeader
                    : (inFlight == 0 && streamSignals.empty())
                        ? ConnectionScanner::Phase::kIdle
                        : ConnectionScanner::Phase::kReadingBody);
            const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
                [&stream, &readBuffer](auto handler) mutable {
                    stream.async_read_some(
                        asio::buffer(readBuffer.data(), readBuffer.size()), std::move(handler));
                });
            if (ec || bytesRead == 0) {
                break;
            }
            scannerEntry.touch();
            // The server has begun draining: tell the peer to stop opening streams
            // (RFC 9113 §6.8); streams already started keep running.
            if (!connection.draining() && env.serverStarted != nullptr &&
                !env.serverStarted->load(std::memory_order_relaxed)) {
                connection.beginDrain();
            }
            (void)connection.feed(std::string_view(readBuffer.data(), bytesRead));
            drainEvents();
            wakeWriter();  // feed may have produced ACKs / WINDOW_UPDATEs to flush
            if (connection.closing() || writeFailed) {
                break;  // closing, or the write side is dead -- stop admitting work
            }
        }
    }

    // Reader done: every stream still waiting on inbound bytes or window space must
    // observe EOF now, or its handler (and thus the writer join below) waits forever.
    for (const auto& entry : streamSignals) {
        entry.second->end();
    }

    // Let the writer drain the last output once every handler has finished, then join
    // it (handlers wake the writer as they complete, so joining the writer waits for
    // them too). Level-triggered: skip the wait entirely if the writer already exited.
    stopping = true;
    wakeWriter();
    while (!writerDone) {
        co_await asyncError([&writerFinished](auto handler) mutable {
            writerFinished.async_wait(std::move(handler));
        });
    }
    co_return;
}

}  // namespace ruvia::detail
