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
// request-body size limits, streaming request bodies (BodyReader over the Web-owned
// per-stream body queue), WebSocket tunnels (RFC 8441), streaming + buffered + file-body
// responses with compression/CORS via prepareBufferedHttpResponse, access logging,
// client certificates, connection-scanner inactivity phases, graceful drain on server
// shutdown, TLS ALPN, and cleartext prior-knowledge startup.
//
// Lifetime safety: a request/response holds VIEWS into its stream's decoded storage, so
// before spawning a handler the stream is pinned (see Http2Connection::pinStream) -- a
// peer RST then keeps the stream alive+reset rather than freeing it, and the handler
// checks isAborted() before submitting. The handler unpins on completion, freeing it.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/web/detail/http/ContextServices.h"
#include "ruvia/web/detail/server/HttpFileOpen.h"
#include "ruvia/web/detail/server/RequestMemoryArena.h"
#include "ruvia/web/detail/body/HttpRequestBodyFacade.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"
#include "ruvia/web/detail/http2/Http2SansIoResponseStreamSink.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/http2/Http2SansIoWsTransport.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/web/detail/server/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/HttpFileChunkBuffer.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/web/detail/server/HttpResponseStreamDispatch.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/RateLimitDecision.h"
#include "ruvia/web/detail/server/RequestBodyLimit.h"
#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/web/detail/websocket/HttpWebSocketSession.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/web/HttpServerOptions.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

// Complete, non-null connection wiring captured by value in the session coroutine.
// Optional product integrations remain explicit inside ContextServices, while
// options, scanner ownership, and graceful-shutdown state are mandatory references.
class Http2SansIoSessionContext final {
public:
    Http2SansIoSessionContext(
        ContextServices services,
        const HttpServerOptions& options,
        ConnectionScanner::Entry& scannerEntry,
        const std::atomic_bool& serverStarted,
        std::string_view remoteAddress,
        std::string_view clientCertificate = {}) noexcept
        : services_(services),
          options_(&options),
          scannerEntry_(&scannerEntry),
          serverStarted_(&serverStarted),
          remoteAddress_(remoteAddress),
          clientCertificate_(clientCertificate) {}

    [[nodiscard]] const HttpServerOptions& options() const noexcept {
        return *options_;
    }

    [[nodiscard]] ConnectionScanner::Entry& scannerEntry() const noexcept {
        return *scannerEntry_;
    }

    [[nodiscard]] const std::atomic_bool& serverStarted() const noexcept {
        return *serverStarted_;
    }

    [[nodiscard]] std::string_view remoteAddress() const noexcept {
        return remoteAddress_;
    }

    [[nodiscard]] RateLimiter* rateLimiter() const noexcept {
        return services_.rateLimiter();
    }

    [[nodiscard]] ContextServices routeServices(bool secure) const noexcept {
        return services_.withTransport(
            remoteAddress_, clientCertificate_, secure);
    }

private:
    ContextServices services_;
    const HttpServerOptions* options_;
    ConnectionScanner::Entry* scannerEntry_;
    const std::atomic_bool* serverStarted_;
    std::string_view remoteAddress_;
    std::string_view clientCertificate_;
};

[[nodiscard]] inline ConnectionScanner::Phase http2SansIoInactivityPhase(
    bool headerBlockInProgress,
    std::size_t activeRuntimeCount) noexcept {
    if (headerBlockInProgress) {
        return ConnectionScanner::Phase::kReadingInitial;
    }
    return activeRuntimeCount == 0
        ? ConnectionScanner::Phase::kIdle
        : ConnectionScanner::Phase::kReadingPayload;
}

template <typename Stream>
Task<void> runHttp2SansIoSession(
    Stream& stream,
    const RouteTable& routes,
    WorkerMemory& worker,
    Http2SansIoSessionContext session,
    std::string_view initialBytes = {}) {
    auto executor = stream.get_executor();
    const auto& options = session.options();
    auto& scannerEntry = session.scannerEntry();
    const auto remoteAddress = session.remoteAddress();
    constexpr bool kTlsStream = !std::is_same_v<Stream, asio::ip::tcp::socket>;

    Http2Connection connection(worker.resource(), Http2Role::kServer);

    asio::steady_timer writeSignal(executor);
    bool stopping = false;  // reader has finished (EOF/error/connection error)
    bool writeFailed = false;
    bool writerDone = false;  // writer coroutine has fully exited (level-triggered join)
    bool initialInputRetained = false;

    const auto wakeWriter = [&writeSignal]() noexcept {
        asio::error_code ignored;
        writeSignal.cancel(ignored);  // wakes async_wait with operation_aborted
    };

    // One stable Web runtime owns every per-stream application concern: route
    // resolution, request-body storage, the dispatch signal, and the dispatch lease.
    // The table's dispatched count is established synchronously at admission, before
    // co_spawn can schedule the handler, and is the writer/join source of truth.
    Http2SansIoStreamRuntimeTable streamRuntimes(worker.resource());
    const auto findStreamRuntime = [&streamRuntimes](
        std::uint32_t streamId) noexcept {
        return streamRuntimes.find(streamId);
    };
    const auto ensureStreamRuntime = [&streamRuntimes](
        std::uint32_t streamId) {
        return streamRuntimes.ensure(streamId);
    };
    const auto eraseStreamRuntime = [&streamRuntimes](
        std::uint32_t streamId) {
        (void)streamRuntimes.remove(streamId);
    };
    const auto findSignal = [&streamRuntimes](
        std::uint32_t streamId) noexcept -> Http2SansIoStreamSignal* {
        auto* runtime = streamRuntimes.find(streamId);
        return runtime != nullptr ? runtime->signal() : nullptr;
    };

    // Single writer: serialize all outbound writes; sleep on writeSignal when idle.
    // The pending bytes are MOVED out before each write (takeOutput): concurrent
    // handlers keep submitting while async_write is in flight, and an append that
    // reallocated the core's buffer would dangle a pendingOutput() view mid-write.
    //
    // CRITICAL: the writer's ONLY exit is
    // `stopping && dispatchedCount == 0`. A write error
    // sets writeFailed but keeps looping (draining and discarding output) until every
    // admitted handler has finished, because the session's teardown join treats the
    // writer's exit as the handler join too -- exiting while dispatchedCount is
    // nonzero would
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
            if (stopping && streamRuntimes.dispatchedCount() == 0) {
                co_return;  // nothing left to write and no more will be produced
            }
            writeSignal.expires_at((asio::steady_timer::time_point::max)());
            const auto waitEc = co_await asyncError([&writeSignal](auto handler) mutable {
                writeSignal.async_wait(std::move(handler));
            });
            (void)waitEc;
        }
    };

    // Wait until the reader reports this stream's window-blocked remainder drained
    // (via takeDrainedDataStreams -> signal wake). Loops on hasQueuedData so a false
    // wake (a request-body chunk, or another stream's unblock) does NOT let the caller
    // read the next body chunk ahead while still blocked -- that would grow the core's
    // pendingSends_ without bound and defeat the backpressure. Returns false (stop) if
    // the stream died or the signal ended (session teardown).
    auto awaitSendWindow = [&](std::uint32_t streamId) -> Task<bool> {
        auto* signal = findSignal(streamId);
        for (;;) {
            auto* live = connection.stream(streamId);
            if (live == nullptr || live->isAborted()) {
                co_return false;
            }
            if (!connection.hasQueuedData(streamId)) {
                co_return true;  // window reopened; the remainder drained
            }
            if (signal == nullptr || signal->ended()) {
                co_return false;
            }
            co_await signal->wait();
        }
    };

    // Submit one stable DATA view with explicit ownership. kQueued means the core
    // copied any unsent suffix, so this call waits for that owned input to drain and
    // never resubmits it. kBackpressured accepted nothing; after the older queued
    // input drains, retry this exact view before its owner advances or refills it.
    auto submitData = [&](std::uint32_t streamId,
                          std::string_view chunk,
                          Http2EndStream endStream) -> Task<bool> {
        for (;;) {
            const auto result = connection.submitData(streamId, chunk, endStream);
            wakeWriter();
            if (result == Http2DataSubmitStatus::kAccepted) {
                co_return true;
            }
            if (result == Http2DataSubmitStatus::kClosed ||
                result == Http2DataSubmitStatus::kInvalidState ||
                result == Http2DataSubmitStatus::kContentLengthExceeded ||
                result == Http2DataSubmitStatus::kContentLengthIncomplete) {
                co_return false;
            }
            if (!(co_await awaitSendWindow(streamId))) {
                co_return false;
            }
            if (result == Http2DataSubmitStatus::kQueued) {
                co_return true;
            }
            // kBackpressured: the input is still caller-owned; retry unchanged.
        }
    };

    // Submit a complete buffered/file response through the core. Explicit response
    // streaming has its own route dispatch and ResponseStreamSink call chain.
    auto submitResponse = [&](std::uint32_t streamId, const HttpResponse& response) -> Task<void> {
        auto* streamState = connection.stream(streamId);
        if (streamState == nullptr || streamState->isAborted()) {
            co_return;
        }
        const auto headResult = connection.submitResponseHead(streamId, response);
        const auto* submittedHead = headResult.submitted();
        if (submittedHead == nullptr) {
            if (headResult.failure()->error() !=
                Http2ResponseHeadSubmitError::kClosed) {
                // A handler supplied metadata that cannot form a conformant final
                // HTTP/2 response (for example 426, which would require forbidden
                // Upgrade metadata). Do not strand the peer waiting forever on an
                // open stream after the transactional head rejection.
                (void)connection.submitReset(
                    streamId,
                    Http2ErrorCode::kInternalError);
                wakeWriter();
            }
            co_return;
        }
        wakeWriter();
        const auto& writePlan = submittedHead->plan();
        if (!writePlan.sendBody()) {
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
                (void)connection.submitReset(streamId, Http2ErrorCode::kInternalError);
                wakeWriter();
                co_return;
            }
            std::pmr::string fileChunk(worker.allocator<char>());
            ensureFileChunkBuffer(fileChunk);
            std::uint64_t remaining = fileBody.length;
            while (remaining > 0) {
                auto* live = connection.stream(streamId);
                if (live == nullptr || live->isAborted()) {
                    co_return;
                }
                const auto next = static_cast<std::size_t>(
                    std::min<std::uint64_t>(fileChunk.size(), remaining));
                input.read(fileChunk.data(), static_cast<std::streamsize>(next));
                const auto readBytes = input.gcount();
                if (readBytes <= 0) {
                    (void)connection.submitReset(streamId, Http2ErrorCode::kInternalError);
                    wakeWriter();
                    co_return;
                }
                remaining -= static_cast<std::uint64_t>(readBytes);
                if (!(co_await submitData(
                    streamId,
                    std::string_view(fileChunk.data(), static_cast<std::size_t>(readBytes)),
                    remaining == 0
                        ? Http2EndStream::kEndStream
                        : Http2EndStream::kKeepOpen))) {
                    co_return;
                }
            }
            co_return;
        }
        // Buffered bytes: pace in frame-sized slices instead of one whole submit. The
        // body view is stable (owned by `response` on this handler frame), so a slow
        // client only ever makes the core buffer at most ONE slice as a window-blocked
        // remainder (vs. a copy of the entire windowed-out tail for a large response).
        const auto body = responseBodyBytes(response);
        constexpr std::size_t kSlice = 16 * 1024;
        std::size_t offset = 0;
        while (offset < body.size()) {
            const auto n = std::min<std::size_t>(kSlice, body.size() - offset);
            const auto endStream = offset + n == body.size()
                ? Http2EndStream::kEndStream
                : Http2EndStream::kKeepOpen;
            if (!(co_await submitData(streamId, body.substr(offset, n), endStream))) {
                co_return;
            }
            offset += n;
        }
        // An empty write plan returned earlier; submitResponseHead END_STREAM'd it.
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
        auto* streamRuntime = findStreamRuntime(streamId);
        if (streamRuntime == nullptr) {
            (void)connection.submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        auto* streamSignal = streamRuntime->signal();
        if (streamSignal == nullptr) {
            (void)connection.submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        const auto baseServices = session.routeServices(kTlsStream);

        HttpRequest request = HttpRequestAccess::make();
        if (!Http2RequestBuilder::build(
                *streamState,
                request,
                requestMemory.resource(),
                streamRuntime->body().streaming()
                    ? std::string_view{}
                    : streamRuntime->body().buffered())) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(400, {}, "invalid http2 request headers"),
                baseServices);
            co_await submitResponse(streamId, response);
            co_return;
        }
        if (streamState->expectationAction() ==
            HttpServerExpectationAction::kUnsupported) {
            // Expect is extensible, so the HTTP/2 decoder accepted and preserved
            // the field. This Web product supports only 100-continue and applies
            // its 417 policy through the normal application error handler.
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(417, {}, "unsupported Expect header"),
                baseServices);
            co_await submitResponse(streamId, response);
            co_return;
        }
        const auto* routeResolution = streamRuntime->routeResolution();
        if (routeResolution == nullptr) {
            (void)connection.submitReset(
                streamId, Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return;
        }
        const auto& resolution = *routeResolution;
        const auto* resolved = resolution.resolved();

        const auto appRateLimit = rateLimitRequestAllowed(
            session.rateLimiter(), remoteAddress);
        if (!appRateLimit.allowed) {
            auto response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(429, {}, "rate limit exceeded"),
                baseServices);
            setRetryAfterSeconds(response, std::chrono::milliseconds(appRateLimit.resetAfterMs));
            co_await submitResponse(streamId, response);
            recordHttpAccess(
                options.accessLog, request, remoteAddress, response.status(), requestStart, true);
            co_return;
        }
        std::optional<Http2SansIoRequestBodyReader> streamReaderStorage;
        std::optional<BodyReader> bodyReaderStorage;
        if (streamRuntime->body().streaming() &&
            streamState->tunnel().pending() == nullptr) {
            streamReaderStorage.emplace(
                connection,
                streamId,
                streamRuntime->body().queue(),
                *streamSignal,
                writeSignal);
            emplaceBodyReaderFacade(bodyReaderStorage, *streamReaderStorage);
        }
        auto dispatchServices = baseServices;
        if (bodyReaderStorage) {
            dispatchServices = dispatchServices.withStreamingRequestBody(*bodyReaderStorage);
        }

        HttpResponse response(requestMemory.resource());
        const auto* webSocketEndpoint = resolved == nullptr
            ? nullptr
            : resolved->route().endpoint().webSocket();
        const auto* responseStreamEndpoint = resolved == nullptr
            ? nullptr
            : resolved->route().endpoint().responseStream();
        if (webSocketEndpoint != nullptr) {
            if (http2IsValidWebSocketRequest(*streamState, request)) {
                // RFC 7692 permessage-deflate over h2 (parity with the h1 handshake).
                const auto deflate = webSocketNegotiatePermessageDeflate(request);
                const auto handshakeStatus = connection.submitWebSocketHandshake(
                    streamId,
                    http2ChooseWebSocketSubprotocol(
                        request, webSocketEndpoint->subprotocols()),
                    webSocketDeflateResponseExtensions(deflate));
                if (handshakeStatus != Http2SubmitStatus::kAccepted) {
                    co_return;
                }
                wakeWriter();  // flush the 200 before the first tunnel read suspends
                // Each Extended-CONNECT tunnel registers its OWN heartbeat on the
                // connection's scanner entry (Entry holds a small set of heartbeat
                // slots, not one), so concurrent tunnels on this h2 connection do not
                // clobber each other's server-initiated pings.
                using WsTransport = Http2SansIoWsTransport<decltype(executor)>;
                WebSocketConnection<WsTransport> webSocketConnection(
                    WsTransport(
                        connection,
                        streamId,
                        streamRuntime->body().queue(),
                        *streamSignal,
                        writeSignal,
                        executor),
                    scannerEntry,
                    webSocketEndpoint->lifecycle(),
                    options.maxWebSocketMessageBytes,
                    requestMemory.resource(),
                    /*initialBytes=*/{},
                    deflate.enabled);
                co_await runWebSocketSession(
                    webSocketConnection,
                    scannerEntry,
                    routes,
                    request,
                    *resolved,
                    requestMemory, baseServices);
                co_return;  // tunnel handled on the wire; no buffered tail (parity)
            }
            response = co_await routes.handleError(
                request, requestMemory,
                HttpErrorInfo(400, {}, "invalid http2 websocket request"),
                baseServices);
        } else if (responseStreamEndpoint != nullptr) {
            // Streaming route (for example SSE): drive the shared streaming
            // dispatch through a sans-I/O sink that submits chunks via the core.
            Http2SansIoResponseStreamSink<decltype(executor)> sink(
                connection,
                streamId,
                responseStreamEndpoint->kind(),
                requestMemory.resource(),
                executor, writeSignal, *streamSignal);
            auto result = co_await dispatchResponseStreamWith(
                sink,
                routes,
                request,
                *resolved,
                requestMemory,
                dispatchServices,
                [&connection, streamId]() noexcept {
                    auto* s = connection.stream(streamId);
                    return s == nullptr || s->isAborted();
                });
            if (result.abortedAfterCommit()) {
                (void)connection.submitReset(streamId, Http2ErrorCode::kInternalError);
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
                request, resolution, requestMemory, dispatchServices);
        }

        auto* live = connection.stream(streamId);
        if (live == nullptr || live->isAborted()) {
            co_return;
        }
        std::pmr::string responseCompressionScratch(
            requestMemory.resource());
        (void)prepareBufferedHttpResponse(
            request,
            response,
            options,
            responseCompressionScratch);
        co_await submitResponse(streamId, response);
        recordHttpAccess(
            options.accessLog, request, remoteAddress, response.status(), requestStart, true);
    };

    // One concurrent handler: admission already owns the table's dispatch lease.
    // Run the dispatch body, then removing the runtime releases route/body/signal and
    // the lease atomically before the protocol pin is released.
    auto dispatchOne = [&](std::uint32_t streamId) -> Task<void> {
        try {
            co_await dispatchOneInner(streamId);
        } catch (...) {
            // Last-resort: a dispatch failure must not leak the pin/dispatch lease.
            auto* live = connection.stream(streamId);
            if (live != nullptr && !live->isAborted()) {
                (void)connection.submitReset(streamId, Http2ErrorCode::kInternalError);
            }
        }
        eraseStreamRuntime(streamId);
        connection.unpinStream(streamId);
        wakeWriter();
        co_return;
    };

    // Owner-side route policy (1:1 port of the coroutine resolveStreamRoute), run at
    // kMessageHead so body-mode/tunnel decisions land BEFORE the next feed.
    const auto resolveStreamRoute = [&routes, &ensureStreamRuntime](
        Http2StreamState& streamState) -> Http2SansIoStreamRuntime* {
        const auto method = Http2RequestBuilder::routeMethod(streamState);
        const auto path = Http2RequestBuilder::requestPath(streamState);
        auto* runtime = ensureStreamRuntime(streamState.id());
        if (runtime == nullptr) {
            return nullptr;
        }
        RouteResolution resolution;
        auto bodyMode = RequestBodyMode::kBuffered;
        if (method != HttpKnownMethod::kUnknown && !path.empty()) {
            resolution = routes.resolve(method, path);
        }
        const auto* resolved = resolution.resolved();
        if (resolved != nullptr) {
            bodyMode = resolved->route().endpoint().requestBodyMode();
        }
        if (http2IsPendingWebSocketConnect(streamState) &&
            resolved != nullptr &&
            resolved->route().endpoint().webSocket() != nullptr) {
            bodyMode = RequestBodyMode::kStream;
        }
        return runtime->selectRoute(std::move(resolution), bodyMode)
            ? runtime
            : nullptr;
    };

    // Admit a stream for dispatch: EVERY dispatched stream gets a signal, so response
    // send-window pacing (awaitSendWindow / the streaming sink) can be woken by
    // takeDrainedDataStreams no matter the body kind -- a plain route returning a large
    // file or a stream body blocks on the send window exactly like a streaming route,
    // and without a signal its blocked submit could never be woken (truncation + hang).
    const auto admitStream = [&](std::uint32_t streamId) {
        if (streamRuntimes.beginDispatch(streamId, executor) == nullptr) {
            return false;
        }
        connection.pinStream(streamId);
        // Recycle the per-stream handler's coroutine frame (mirrors the h1 accept path);
        // under load the common request then does no extra heap work for the frame.
        asio::co_spawn(
            executor, taskAsAwaitable(dispatchOne(streamId)),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
        return true;
    };

    // Drain the core's event queue, admitting streams and routing body bytes.
    const auto drainEvents = [&]() {
        std::array<std::uint32_t,
                   Http2LocalSettings::kMaxConcurrentStreams>
            copiedBodyStreams{};
        std::size_t copiedBodyStreamCount = 0;
        const auto markBufferedBodyCopied = [&](std::uint32_t streamId) {
            const auto end = copiedBodyStreams.begin() +
                static_cast<std::ptrdiff_t>(copiedBodyStreamCount);
            if (std::find(copiedBodyStreams.begin(), end, streamId) == end) {
                if (copiedBodyStreamCount == copiedBodyStreams.size()) {
                    return false;
                }
                copiedBodyStreams[copiedBodyStreamCount++] = streamId;
            }
            return true;
        };
        const auto unmarkBufferedBodyCopied = [&](std::uint32_t streamId) {
            const auto end = copiedBodyStreams.begin() +
                static_cast<std::ptrdiff_t>(copiedBodyStreamCount);
            const auto found = std::find(
                copiedBodyStreams.begin(), end, streamId);
            if (found == end) {
                return;
            }
            --copiedBodyStreamCount;
            *found = copiedBodyStreams[copiedBodyStreamCount];
        };
        const auto resetEventStream = [&](std::uint32_t streamId,
                                          Http2ErrorCode error) {
            unmarkBufferedBodyCopied(streamId);
            auto* signal = findSignal(streamId);
            (void)connection.submitReset(streamId, error);
            if (signal != nullptr) {
                // A dispatched handler owns the runtime until dispatchOne finishes.
                signal->wake();
            } else {
                // Owner-side reset is intentionally not echoed as kStreamClosed.
                // An undispatched runtime therefore has to be reclaimed here.
                eraseStreamRuntime(streamId);
            }
            wakeWriter();
        };
        for (;;) {
            const auto event = connection.nextEvent();
            if (!event.has_value()) {
                break;
            }
            if (const auto* messageHead = event->messageHead()) {
                const auto streamId = messageHead->streamId();
                auto* streamState = connection.stream(streamId);
                if (streamState == nullptr) {
                    continue;
                }
                auto* streamRuntime = resolveStreamRoute(*streamState);
                if (streamRuntime == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                const auto expectationAction = streamState->expectationAction();
                if (expectationAction ==
                    HttpServerExpectationAction::kSend100Continue) {
                    const auto status = connection.submitInterimResponseHead(
                        streamId,
                        HttpInterimResponseHead(100));
                    if (status == Http2SubmitStatus::kAccepted) {
                        wakeWriter();
                    } else {
                        if (status != Http2SubmitStatus::kClosed) {
                            resetEventStream(
                                streamId, Http2ErrorCode::kInternalError);
                        } else {
                            eraseStreamRuntime(streamId);
                        }
                        continue;
                    }
                }
                const bool connectRequest =
                    streamState->tunnel().pending() != nullptr;
                const bool streamingBody = !connectRequest &&
                    streamRuntime->body().streaming() &&
                    streamState->remoteReceive().contentOpen() != nullptr;
                if (expectationAction ==
                        HttpServerExpectationAction::kUnsupported ||
                    connectRequest || streamingBody) {
                    // Dispatch NOW; body bytes stream through the Web runtime's queue
                    // while the handler runs. Unsupported expectations also need an
                    // immediate 417: waiting for buffered content would deadlock a
                    // conforming client that is waiting for the expectation decision.
                    if (!admitStream(streamId)) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                }
            } else if (const auto* bodyChunk = event->messageBodyChunk()) {
                const auto streamId = bodyChunk->streamId();
                auto* streamState = connection.stream(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                if (streamState == nullptr || streamRuntime == nullptr) {
                    if (streamState != nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                    continue;
                }
                const auto* bodyMode =
                    streamRuntime->body().selectedMode();
                if (bodyMode == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                const auto totalLimit = requestBodyByteLimit(
                    *bodyMode,
                    options.maxStreamBodyBytes,
                    options.maxBufferedBodyBytes);
                const auto stored = streamRuntime->body().store(
                    bodyChunk->bytes(),
                    totalLimit,
                    options.maxBufferedBodyBytes);
                if (stored != Http2RequestBodyStoreResult::kAccepted) {
                    resetEventStream(
                        streamId,
                        stored == Http2RequestBodyStoreResult::kModeNotSelected
                            ? Http2ErrorCode::kInternalError
                            : Http2ErrorCode::kCancel);
                    continue;
                }
                if (streamRuntime->body().streaming()) {
                    auto* signal = streamRuntime->signal();
                    if (signal == nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                        continue;
                    }
                    signal->wake();
                } else {
                    // Delay acknowledgement until the complete event batch has been
                    // copied. releaseReceivedData() returns all debt currently held
                    // by the stream, including later DATA frames from this feed.
                    if (!markBufferedBodyCopied(streamId)) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                }
            } else if (const auto* tunnelData = event->tunnelData()) {
                const auto streamId = tunnelData->streamId();
                auto* streamState = connection.stream(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                auto* signal = streamRuntime != nullptr
                    ? streamRuntime->signal()
                    : nullptr;
                if (streamState == nullptr || streamRuntime == nullptr ||
                    signal == nullptr) {
                    if (streamState != nullptr) {
                        resetEventStream(
                            streamId, Http2ErrorCode::kInternalError);
                    }
                    continue;
                }
                streamRuntime->body().queue().enqueue(tunnelData->bytes());
                signal->wake();
            } else if (const auto* tunnelEnd = event->tunnelEnd()) {
                if (auto* signal = findSignal(tunnelEnd->streamId())) {
                    signal->wake();
                }
            } else if (const auto* messageEnd = event->messageEnd()) {
                const auto streamId = messageEnd->streamId();
                if (connection.stream(streamId) == nullptr) {
                    continue;
                }
                auto* streamRuntime = findStreamRuntime(streamId);
                if (streamRuntime == nullptr) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                    continue;
                }
                if (auto* signal = streamRuntime->signal()) {
                    signal->wake();  // remote END_STREAM was committed before this event
                } else if (!admitStream(streamId)) {
                    resetEventStream(
                        streamId, Http2ErrorCode::kInternalError);
                }
            } else if (const auto* streamClosed = event->streamClosed()) {
                const auto streamId = streamClosed->streamId();
                unmarkBufferedBodyCopied(streamId);
                auto* streamRuntime = findStreamRuntime(streamId);
                auto* signal = streamRuntime != nullptr
                    ? streamRuntime->signal()
                    : nullptr;
                if (signal != nullptr) {
                    signal->wake();  // stream is reset; blocked readers/writers see it
                } else {
                    // The protocol core can erase an unpinned reset stream before
                    // this event is drained. Cleanup is keyed by the typed event,
                    // not by a second lookup of already-removed core state.
                    eraseStreamRuntime(streamId);
                }
            }
        }
        for (std::size_t i = 0; i < copiedBodyStreamCount; ++i) {
            connection.releaseReceivedData(copiedBodyStreams[i]);
        }
        if (copiedBodyStreamCount != 0) {
            wakeWriter();
        }
        // Send-window reopenings drained deferred bodies inside the core; wake the
        // paced response writers so they pull their next chunk.
        for (const auto streamId : connection.takeDrainedDataStreams()) {
            if (auto* signal = findSignal(streamId)) {
                signal->wake();
            }
        }
    };

    // One ownership path for every inbound span. The core can refuse a span while an
    // earlier zero-copy event is pending; drain that event queue, then retry the exact
    // same bytes. The enum is the entire ownership result: no byte-count side channel
    // exists because the core never partially accepts a span.
    const auto feedAndDrain = [&](std::string_view bytes) {
        for (;;) {
            const auto result = connection.feed(bytes);
            drainEvents();
            if (result != Http2FeedResult::kEventsPending) {
                return result;
            }
        }
    };

    // --- startup ----------------------------------------------------------------
    connection.beginConnection();

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
            asio::error_code ignored;
            writerFinished.cancel(ignored);
        });

    // Establish the feed contract: drain any startup events before initial input.
    drainEvents();
    if (!connection.connectionError().has_value() && !initialBytes.empty()) {
        const auto result = feedAndDrain(initialBytes);
        initialInputRetained =
            result == Http2FeedResult::kConnectionNotStarted;
    }
    wakeWriter();

    // Reader loop: feed inbound bytes, then act on the drained events.
    if (!connection.connectionError().has_value() && !initialInputRetained) {
        std::array<char, 16384> readBuffer;
        for (;;) {
            // Pick the inactivity phase: mid-header-block -> the tight header timeout;
            // no active Web runtime (including a pre-dispatch buffered request body)
            // -> kIdle so a keep-alive connection between requests is governed by the
            // keepalive timeout, NOT client_body_timeout; otherwise a body/response is
            // in progress -> kReadingPayload. (WS tunnels carry their own heartbeat.)
            scannerEntry.setPhase(http2SansIoInactivityPhase(
                connection.headerBlockInProgress(),
                streamRuntimes.size()));
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
            if (!connection.draining() &&
                !session.serverStarted().load(std::memory_order_relaxed)) {
                connection.beginDrain();
            }
            const auto result = feedAndDrain(
                std::string_view(readBuffer.data(), bytesRead));
            wakeWriter();  // feed may have produced ACKs / WINDOW_UPDATEs to flush
            if (result == Http2FeedResult::kConnectionNotStarted ||
                result == Http2FeedResult::kProtocolFailure || writeFailed) {
                break;  // retained input, terminal protocol failure, or dead write side
            }
        }
    }

    // Reader done: every stream still waiting on inbound bytes or window space must
    // observe EOF now, or its handler (and thus the writer join below) waits forever.
    streamRuntimes.forEach([](Http2SansIoStreamRuntime& runtime) {
        if (auto* signal = runtime.signal()) {
            signal->end();
        }
    });

    // Let the writer drain the last output once every handler has finished, then join
    // it (handlers wake the writer as they complete, so joining the writer waits for
    // them too). Level-triggered: skip the wait entirely if the writer already exited.
    stopping = true;
    wakeWriter();
    while (!writerDone) {
        const auto waitEc = co_await asyncError([&writerFinished](auto handler) mutable {
            writerFinished.async_wait(std::move(handler));
        });
        (void)waitEc;
    }
    co_return;
}

}  // namespace ruvia::detail
