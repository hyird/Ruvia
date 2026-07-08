#pragma once

// Concurrent buffered HTTP/2 server session over the sans-I/O core (ruvia-web).
//
// Drives an Http2Connection with a reader coroutine + a single writer coroutine so that
// HTTP/2 streams are genuinely multiplexed: each completed request is dispatched on its
// own concurrent handler task, and a slow handler never blocks reading or the other
// streams. All outbound bytes funnel through the one writer (submit* only appends to the
// connection's buffer, so ordering is automatic); the writer sleeps on a signal timer
// when idle and is woken whenever new output is produced.
//
// Lifetime safety: a request/response holds VIEWS into its stream's decoded storage, so
// before spawning a handler the stream is pinned (see Http2Connection::pinStream) -- a
// peer RST then keeps the stream alive+reset rather than freeing it, and the handler
// checks isReset() before submitting. The handler unpins on completion, freeing it.
//
// SCOPE (additive, not yet wired into the accept loop): buffered requests + buffered
// responses. Streaming responses, WebSocket tunnels, streaming request bodies, rate
// limiting, body-size limits and access logging remain to reach full parity with the
// coroutine Http2ServerSession.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>

#include "HttpRequestInternal.h"
#include "HttpResponseBodyAccess.h"
#include "http/ContextServices.h"
#include "net/http2/Http2Connection.h"
#include "net/http2/Http2RequestBuilder.h"
#include "net/http2/Http2SansIoResponseStreamSink.h"
#include "net/server/HttpResponseStreamDispatch.h"
#include "router/RouteResolution.h"
#include "router/RouteTable.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
Task<void> runHttp2SansIoBufferedSession(
    Stream& stream, const RouteTable& routes, WorkerMemory& worker, std::string_view remoteAddress) {
    auto executor = stream.get_executor();
    Http2Connection connection(worker.resource());
    connection.expectClientPreface();
    connection.queueLocalSettings();

    asio::steady_timer writeSignal(executor);
    int inFlight = 0;       // concurrent handlers not yet finished
    bool stopping = false;  // reader has finished (EOF/error/closing)
    bool writeFailed = false;

    const auto wakeWriter = [&writeSignal]() noexcept {
        writeSignal.cancel();  // wakes async_wait with operation_aborted
    };

    // Single writer: serialize all outbound writes; sleep on writeSignal when idle.
    auto writerLoop = [&]() -> Task<void> {
        for (;;) {
            while (connection.wantsWrite()) {
                const auto out = connection.pendingOutput();
                const auto ec = co_await asyncError([&stream, out](auto handler) mutable {
                    asio::async_write(
                        stream, asio::buffer(out.data(), out.size()), std::move(handler));
                });
                if (ec) {
                    writeFailed = true;
                    co_return;
                }
                connection.consumeOutput(out.size());
            }
            if ((stopping && inFlight == 0) || writeFailed) {
                co_return;  // nothing left to write and no more will be produced
            }
            writeSignal.expires_at((asio::steady_timer::time_point::max)());
            co_await asyncError([&writeSignal](auto handler) mutable {
                writeSignal.async_wait(std::move(handler));
            });
        }
    };

    // One concurrent handler: build the request from the (pinned) stream, dispatch, and
    // submit the response unless the stream was reset while the handler ran.
    auto dispatchOne = [&](std::uint32_t streamId) -> Task<void> {
        ++inFlight;
        {
            RequestMemory requestMemory(worker);
            HttpRequest request = HttpRequestAccess::make();
            auto* streamState = connection.stream(streamId);
            if (streamState != nullptr &&
                Http2RequestBuilder::build(*streamState, request, requestMemory.resource())) {
                HttpRequestAccess::setTransport(request, remoteAddress, std::string_view{}, false);
                RouteMatch match;
                const auto resolution = routes.resolve(request, match);
                const bool bodyForbidden = request.method() == HttpMethod::kHead;

                if (resolution.usesResponseStream()) {
                    // Streaming route (Context::proxy / SSE): drive the shared streaming
                    // dispatch through a sans-I/O sink that submits chunks via the core.
                    Http2SansIoResponseStreamSink<decltype(executor)> sink(
                        connection, streamId, resolution.responseMode(), requestMemory.resource(),
                        executor);
                    auto result = co_await dispatchResponseStreamWith(
                        sink, routes, request, resolution, requestMemory, ContextServices{},
                        /*closeConnectionOnError=*/false,
                        [&connection, streamId]() noexcept {
                            auto* s = connection.stream(streamId);
                            return s == nullptr || s->isReset();
                        });
                    if (result.abortedAfterCommit()) {
                        connection.submitReset(
                            streamId, static_cast<std::uint32_t>(Http2ErrorCode::kInternalError));
                    } else if (result.hasBufferedResponse()) {
                        HttpResponse response = result.takeResponse();
                        auto* live = connection.stream(streamId);
                        if (live != nullptr && !live->isReset()) {
                            connection.submitResponseHead(streamId, response, bodyForbidden);
                            connection.submitData(streamId, responseBodyBytes(response), true);
                        }
                    }
                } else {
                    HttpResponse response = co_await routes.dispatchBuffered(
                        request, resolution, requestMemory, /*closeConnectionOnError=*/false,
                        ContextServices{});
                    auto* live = connection.stream(streamId);
                    if (live != nullptr && !live->isReset()) {
                        connection.submitResponseHead(streamId, response, bodyForbidden);
                        connection.submitData(streamId, responseBodyBytes(response), /*endStream=*/true);
                    }
                }
            }
        }
        connection.unpinStream(streamId);
        --inFlight;
        wakeWriter();
        co_return;
    };

    // Spawn the writer; it runs concurrently with the reader below. A cancel of
    // writerFinished latches the writer's exit so we can join it before returning.
    asio::steady_timer writerFinished(executor);
    writerFinished.expires_at((asio::steady_timer::time_point::max)());
    asio::co_spawn(
        executor, taskAsAwaitable(writerLoop()),
        [&writerFinished](std::exception_ptr) noexcept { writerFinished.cancel(); });

    // Reader loop: feed inbound bytes, then act on the drained events.
    std::array<char, 16384> readBuffer;
    for (;;) {
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [&stream, &readBuffer](auto handler) mutable {
                stream.async_read_some(
                    asio::buffer(readBuffer.data(), readBuffer.size()), std::move(handler));
            });
        if (ec || bytesRead == 0) {
            break;
        }
        (void)connection.feed(std::string_view(readBuffer.data(), bytesRead));
        for (;;) {
            const auto event = connection.nextEvent();
            if (event.kind == Http2Event::Kind::kNone) {
                break;
            }
            auto* streamState = connection.stream(event.streamId);
            if (streamState == nullptr) {
                continue;
            }
            if (event.kind == Http2Event::Kind::kRequestBodyChunk) {
                streamState->appendRequestBody(event.bytes);
            } else if (event.kind == Http2Event::Kind::kRequestEnd) {
                connection.pinStream(event.streamId);
                asio::co_spawn(
                    executor, taskAsAwaitable(dispatchOne(event.streamId)), asio::detached);
            }
        }
        wakeWriter();  // feed may have produced ACKs / WINDOW_UPDATEs to flush
        if (connection.closing()) {
            break;
        }
    }

    // Reader done: let the writer drain the last output once every handler has finished,
    // then join it (handlers wake the writer as they complete, so joining the writer
    // waits for them too).
    stopping = true;
    wakeWriter();
    co_await asyncError([&writerFinished](auto handler) mutable {
        writerFinished.async_wait(std::move(handler));
    });
    co_return;
}

}  // namespace ruvia::detail
