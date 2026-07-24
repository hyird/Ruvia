#pragma once

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/edge/detail/http2/Http2ResponseWriter.h"
#include "ruvia/edge/detail/server/ServerImpl.h"
#include "ruvia/edge/detail/server/SessionLimits.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2Event.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"

// The HTTP/2 client session: one reader loop draining connection events, one
// writer coroutine owning all output, and one detached handler per stream. A
// template because the same driver serves a plain and a TLS stream; the caller
// picks by ALPN.

namespace ruvia::edge {

template <typename Stream>
asio::awaitable<void> EdgeServer::Impl::handleHttp2Session(Stream stream, std::string clientAddress) {
    using namespace asio::experimental::awaitable_operators;
    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;

    std::pmr::unsynchronized_pool_resource resource;
    detail::Http2Connection connection(&resource, detail::Http2Role::kServer);
    connection.beginConnection();

    asio::steady_timer writeWake(executor);
    // Woken when a handler finishes, so the draining reader can notice the last
    // in-flight stream completed even while it is blocked awaiting client frames.
    asio::steady_timer handlersIdle(executor);
    std::pmr::unordered_map<std::uint32_t, asio::steady_timer*> drainWaiters(memory_.resource());
    bool shuttingDown = false;
    int activeHandlers = 0;
    // Per-stream cancellation signals for the detached response handlers. On
    // teardown beginShutdown() emits terminal cancellation on each, so a handler
    // parked somewhere it does not otherwise observe the shutdown -- the request-
    // coalescing wait, or an in-flight origin fetch -- is released and unwinds.
    // Node-based storage: cancellation_signal is not movable, and the slot a
    // spawned handler binds must stay valid until that handler completes.
    std::pmr::unordered_map<std::uint32_t, asio::cancellation_signal> handlerCancels(memory_.resource());
    Http2SessionShared shared{connection, writeWake, drainWaiters, shuttingDown};

    // Wake the writer and, once tearing down, release every parked handler so it
    // can observe the shutdown and unwind rather than await a window forever.
    const auto beginShutdown = [&]() {
        shuttingDown = true;
        for (auto& [id, timer] : drainWaiters) {
            timer->cancel();
        }
        // Release every still-running handler wherever it is parked (coalescing
        // wait, origin fetch, disk lookup) so none resumes into the locals below
        // after this frame returns. Cancellation posts the abort, so a handler's
        // completion callback -- which erases from handlerCancels -- runs later,
        // not re-entrantly during this loop.
        for (auto& [id, signal] : handlerCancels) {
            signal.emit(asio::cancellation_type::terminal);
        }
        writeWake.cancel();
    };

    // One stream's response handler: build the logical request from the pinned
    // stream and drive the serve core with an HTTP/2 writer. Named-local so its
    // closure outlives the coroutines co_spawn()ed from it.
    auto serveStream = [this, &shared, resource = &resource, clientAddress = std::string_view(clientAddress)](std::uint32_t streamId, detail::Http2StreamState& streamState, std::pmr::string requestBody) -> asio::awaitable<void> {
        HttpRequest httpRequest = detail::HttpRequestAccess::make();
        const auto buildResult = detail::Http2RequestBuilder::build(streamState, httpRequest, resource, requestBody);
        if (buildResult.built() == nullptr) {
            (void)shared.connection.submitReset(streamId, detail::Http2ErrorCode::kProtocolError);
            shared.writeWake.cancel();
            co_return;
        }

        EdgeRequest edgeRequest;
        edgeRequest.method = httpRequest.method();
        edgeRequest.knownMethod = httpRequest.knownMethod();
        edgeRequest.target = httpRequest.target();
        edgeRequest.host = streamState.requestAuthority();
        edgeRequest.headers = httpRequest.headers();
        edgeRequest.clientAddress = clientAddress;
        edgeRequest.keepAlive = true;
        if (edgeRequest.knownMethod != HttpKnownMethod::kGet && edgeRequest.knownMethod != HttpKnownMethod::kHead && !requestBody.empty()) {
            edgeRequest.body = std::string_view(requestBody);
        }

        Http2ResponseWriter writer(shared, streamId, edgeRequest.knownMethod, resource);
        (void)co_await serveRequest(edgeRequest, writer);
        // If the serve core returned without completing the response (client gone
        // mid-stream), reset the stream so it does not dangle.
        if (!writer.ended()) {
            auto* s = shared.connection.stream(streamId);
            if (s != nullptr && !s->isAborted()) {
                (void)shared.connection.submitReset(streamId, detail::Http2ErrorCode::kInternalError);
            }
        }
        shared.writeWake.cancel();
    };

    // Writer coroutine: the sole owner of async_write. It drains the connection's
    // pending output, then parks on writeWake until more is produced. It exits once
    // the session is shutting down and no handler is still running, or on a fatal
    // connection error, or if a write fails.
    auto writer = [&]() -> asio::awaitable<void> {
        for (;;) {
            while (connection.wantsWrite()) {
                std::pmr::string out(&resource);
                connection.takeOutput(out);
                auto [ec, n] = co_await asio::async_write(stream, asio::buffer(out.data(), out.size()), tuple);
                (void)n;
                if (ec) {
                    beginShutdown();
                    co_return;
                }
            }
            if (connection.connectionError().has_value() || (shuttingDown && activeHandlers == 0)) {
                co_return;
            }
            writeWake.expires_at((std::chrono::steady_clock::time_point::max)());
            co_await writeWake.async_wait(tuple);
        }
    };

    // Reader loop: read, feed, dispatch each completed request to its own handler
    // coroutine so a slow origin on one stream never blocks the others.
    std::array<char, 16384> readBuffer;
    std::pmr::unordered_map<std::uint32_t, std::pmr::string> requestBodies(memory_.resource());

    auto reader = [&]() -> asio::awaitable<void> {
        for (;;) {
            std::size_t readSize = 0;
            asio::error_code readError;
            auto raced = co_await (stream.async_read_some(asio::buffer(readBuffer), tuple) || shutdownSignal_.async_wait(tuple));
            if (raced.index() == 1) {
                break;
            }
            std::tie(readError, readSize) = std::get<0>(raced);
            if (readError) {
                break;
            }
            (void)connection.feed(std::string_view(readBuffer.data(), readSize));
            writeWake.cancel();  // feed may have queued control frames

            // DATA events borrow the current input span and retain receive-window
            // debt. Copy the whole event batch first, then return each stream's
            // accumulated credit exactly once; acknowledging inside the loop
            // could cover later events whose borrowed bytes are not copied yet.
            std::array<std::uint32_t, detail::Http2LocalSettings::kMaxConcurrentStreams> copiedBodyStreams{};
            std::size_t copiedBodyStreamCount = 0;
            const auto markBodyCopied = [&](std::uint32_t streamId) {
                const auto copied = std::span(copiedBodyStreams).first(copiedBodyStreamCount);
                if (std::ranges::find(copied, streamId) != copied.end()) {
                    return true;
                }
                if (copiedBodyStreamCount == copiedBodyStreams.size()) {
                    return false;
                }
                copiedBodyStreams[copiedBodyStreamCount++] = streamId;
                return true;
            };
            const auto unmarkBodyCopied = [&](std::uint32_t streamId) {
                auto copied = std::span(copiedBodyStreams).first(copiedBodyStreamCount);
                const auto found = std::ranges::find(copied, streamId);
                if (found == copied.end()) {
                    return;
                }
                --copiedBodyStreamCount;
                *found = copiedBodyStreams[copiedBodyStreamCount];
            };
            const auto resetBodyStream = [&](std::uint32_t streamId, detail::Http2ErrorCode error) {
                unmarkBodyCopied(streamId);
                requestBodies.erase(streamId);
                (void)connection.submitReset(streamId, error);
                writeWake.cancel();
            };

            for (;;) {
                const auto event = connection.nextEvent();
                if (!event.has_value()) {
                    break;
                }
                if (const auto* head = event->messageHead()) {
                    const auto streamId = head->streamId();
                    const auto* streamState = connection.stream(streamId);
                    const auto* knownLength = streamState == nullptr ? nullptr : streamState->remoteContent().allowedKnownLength();
                    if (knownLength != nullptr && knownLength->declaredLength() > kMaxRequestBytes) {
                        resetBodyStream(streamId, detail::Http2ErrorCode::kCancel);
                    } else {
                        requestBodies.try_emplace(streamId);
                    }
                } else if (const auto* chunk = event->messageBodyChunk()) {
                    const auto streamId = chunk->streamId();
                    const auto body = requestBodies.find(streamId);
                    if (body == requestBodies.end()) {
                        resetBodyStream(streamId, detail::Http2ErrorCode::kInternalError);
                        continue;
                    }
                    if (chunk->bytes().size() > kMaxRequestBytes - body->second.size()) {
                        resetBodyStream(streamId, detail::Http2ErrorCode::kCancel);
                        continue;
                    }
                    body->second.append(chunk->bytes());
                    if (!markBodyCopied(streamId)) {
                        resetBodyStream(streamId, detail::Http2ErrorCode::kInternalError);
                    }
                } else if (const auto* end = event->messageEnd()) {
                    const auto streamId = end->streamId();
                    auto* streamState = connection.stream(streamId);
                    if (streamState == nullptr) {
                        continue;
                    }
                    std::pmr::string body(memory_.resource());
                    if (auto it = requestBodies.find(streamId); it != requestBodies.end()) {
                        body = std::move(it->second);
                        requestBodies.erase(it);
                    }
                    // Register cancellation before acquiring the stream lease.
                    // If allocation or co_spawn() fails synchronously, roll back
                    // every piece of bookkeeping: otherwise the draining loop
                    // below would wait forever for a handler that never started.
                    auto [cancelIt, inserted] = handlerCancels.try_emplace(streamId);
                    if (!inserted) {
                        throw std::logic_error("duplicate HTTP/2 handler for one stream");
                    }
                    bool handlerRegistered = false;
                    try {
                        // Pin so the stream's request/response storage outlives
                        // the detached handler; unpin on its completion.
                        connection.pinStream(streamId);
                        ++activeHandlers;
                        handlerRegistered = true;
                        asio::co_spawn(executor, serveStream(streamId, *streamState, std::move(body)), asio::bind_cancellation_slot(cancelIt->second.slot(), [&, streamId](std::exception_ptr failure) {
                            if (failure != nullptr && !shuttingDown) {
                                auto* failedStream = connection.stream(streamId);
                                if (failedStream != nullptr && !failedStream->isAborted()) {
                                    // A handler that unwinds without a
                                    // terminal response still owes the
                                    // peer a stream terminal state.
                                    (void)connection.submitReset(streamId, detail::Http2ErrorCode::kInternalError);
                                }
                            }
                            // Resetting the stream tells the peer, not
                            // the operator: report what went wrong too.
                            if (failure != nullptr && !isCancellationUnwind(failure)) {
                                reportFailure(EdgeTaskKind::kSession, failure);
                            }
                            connection.unpinStream(streamId);
                            drainWaiters.erase(streamId);
                            handlerCancels.erase(streamId);
                            --activeHandlers;
                            writeWake.cancel();     // let the writer re-check its exit
                            handlersIdle.cancel();  // wake a draining reader
                        }));
                    } catch (...) {
                        if (handlerRegistered) {
                            --activeHandlers;
                            connection.unpinStream(streamId);
                        }
                        handlerCancels.erase(cancelIt);
                        throw;
                    }
                } else if (const auto* closed = event->streamClosed()) {
                    // The peer reset/closed the stream: wake its parked handler so
                    // it sees the abort and unwinds.
                    unmarkBodyCopied(closed->streamId());
                    if (const auto it = drainWaiters.find(closed->streamId()); it != drainWaiters.end()) {
                        it->second->cancel();
                    }
                    requestBodies.erase(closed->streamId());
                }
            }

            for (std::size_t index = 0; index < copiedBodyStreamCount; ++index) {
                connection.releaseReceivedData(copiedBodyStreams[index]);
            }
            if (copiedBodyStreamCount != 0) {
                writeWake.cancel();
            }

            // Resume any handler whose flow-control window just reopened.
            for (const std::uint32_t id : connection.takeDrainedDataStreams()) {
                if (const auto it = drainWaiters.find(id); it != drainWaiters.end()) {
                    it->second->cancel();
                }
            }
            writeWake.cancel();  // handlers may have produced output

            if (connection.connectionError().has_value()) {
                break;
            }
        }
        beginShutdown();
    };

    std::exception_ptr sessionFailure;
    try {
        co_await (reader() && writer());
    } catch (...) {
        // A synchronous throw (e.g. bad_alloc from the writer's output buffer or
        // the reader's body accumulation) can escape the group while a detached
        // handler is still awaiting its origin fetch. Signal teardown so every
        // handler unwinds, then fall through to join them before the locals they
        // captured by reference are destroyed. The failure is held, not
        // discarded: it is rethrown once the join below makes that safe.
        sessionFailure = std::current_exception();
        beginShutdown();
    }

    // The reader and writer have both finished. A connection error (or the throw
    // above) can end them while a per-stream handler is still awaiting its origin
    // fetch or coalescing on another stream; those handlers captured this frame's
    // locals by reference. beginShutdown() has run, so shuttingDown is set and
    // each remaining handler's current await was cancelled -- it unwinds without
    // re-parking. Wait for the last one before destroying the locals.
    while (activeHandlers > 0) {
        handlersIdle.expires_at((std::chrono::steady_clock::time_point::max)());
        co_await handlersIdle.async_wait(tuple);
    }

    asio::error_code ignore;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignore);

    // Every handler has been joined and the transport is closed, so unwinding
    // can no longer strand a coroutine on this frame's locals. Hand the failure
    // to the session's spawn completion, which reports it.
    if (sessionFailure != nullptr) {
        std::rethrow_exception(sessionFailure);
    }
}

}  // namespace ruvia::edge
