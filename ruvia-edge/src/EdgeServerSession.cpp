#include "ruvia/edge/detail/EdgeServerImpl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/ssl.h>  // negotiated ALPN read-back

#include "ruvia/edge/detail/EdgeHeaderRules.h"
#include "ruvia/edge/detail/EdgeHttp1ResponseWriter.h"
#include "ruvia/edge/detail/EdgeHttp1Wire.h"
#include "ruvia/edge/detail/EdgeHttp2ResponseWriter.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2Event.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"

namespace ruvia::edge {

namespace {

// Upper bound on a whole buffered client request (head plus any forwarded body).
constexpr std::size_t kMaxRequestBytes = 1u * 1024u * 1024u;

// How long a persistent client connection may sit idle awaiting its next request.
constexpr std::chrono::seconds kKeepAliveIdleTimeout{60};

}  // namespace

asio::awaitable<void> EdgeServer::Impl::acceptLoop() {
    for (;;) {
        auto [ec, socket] =
            co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                break;
            }
            continue;
        }
        auto tlsContext = loadTlsContext();
        if (tlsContext != nullptr) {
            spawnTracked(
                handleTlsSession(std::move(socket), std::move(tlsContext)));
        } else {
            spawnTracked(handleSession(std::move(socket)));
        }
    }
}

asio::awaitable<void> EdgeServer::Impl::handleTlsSession(
    asio::ip::tcp::socket socket,
    TlsContextPtr context) {
    // The accept loop pins the context for this session's lifetime; a runtime
    // rotation only affects connections accepted afterward.
    asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), *context);
    auto [ec] = co_await stream.async_handshake(
        asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
    if (ec) {
        asio::error_code ignore;
        stream.lowest_layer().close(ignore);
        co_return;
    }

    // Dispatch to HTTP/2 when ALPN negotiated it, otherwise HTTP/1.1.
    const unsigned char* protocol = nullptr;
    unsigned int protocolLength = 0;
    SSL_get0_alpn_selected(stream.native_handle(), &protocol, &protocolLength);
    if (protocolLength == 2 && protocol != nullptr && std::memcmp(protocol, "h2", 2) == 0) {
        std::string clientAddress;
        asio::error_code addressError;
        const auto remote = stream.lowest_layer().remote_endpoint(addressError);
        if (!addressError) {
            clientAddress = remote.address().to_string();
        }
        co_await handleHttp2Session(std::move(stream), std::move(clientAddress));
        co_return;
    }
    co_await handleSession(std::move(stream));
}

template <typename Stream>
asio::awaitable<void> EdgeServer::Impl::handleSession(Stream stream) {
    using namespace asio::experimental::awaitable_operators;

    std::string clientAddress;
    {
        asio::error_code ec;
        const auto remote = stream.lowest_layer().remote_endpoint(ec);
        if (!ec) {
            clientAddress = remote.address().to_string();
        }
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto writeStatus = [&stream, tuple](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(stream, asio::buffer(wire.data(), wire.size()), tuple);
    };

    std::string inbound;
    std::array<char, 8192> buffer;
    const detail::Http1ServerRequestParser parser;
    asio::steady_timer idleTimer(ioContext_);

    // Serve requests on this connection until one closes it, the client goes
    // away, or the connection sits idle past the keep-alive timeout.
    bool keepGoing = true;
    while (keepGoing) {
        std::size_t consumed = 0;
        bool framed = false;
        for (;;) {
            auto parseState = parser.parseMessage(inbound);
            if (const auto* failure = parseState.failure()) {
                const auto protocolError = failure->protocolError();
                co_await writeStatus(buildStatusWire(
                    protocolError.status().value(),
                    parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            const auto* needBody = parseState.needRequestBody();
            const auto* ready = parseState.messageReady();
            const bool exceedsEdgeRequestLimit =
                (needBody != nullptr &&
                 needBody->requiredTotalBytes().has_value() &&
                 *needBody->requiredTotalBytes() > kMaxRequestBytes) ||
                (ready != nullptr &&
                 ready->messageBytes() > kMaxRequestBytes);
            if (exceedsEdgeRequestLimit) {
                // Apply the edge product's tighter buffered-request policy to
                // parser metadata before waiting for or dispatching the body.
                // Checking only inbound.size() after messageReady lets the read
                // that completes an oversized request jump over the limit.
                co_await writeStatus(buildStatusWire(
                    413, parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            if (ready != nullptr) {
                consumed = ready->messageBytes();
                const auto wireBody = std::string_view(inbound).substr(
                    ready->headerBytes(),
                    ready->messageBytes() - ready->headerBytes());
                keepGoing = co_await handleFramedRequest(
                    stream, parseState, wireBody, clientAddress);
                framed = true;
                break;
            }
            if (inbound.size() > kMaxRequestBytes) {
                co_await writeStatus(buildStatusWire(
                    413, parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            idleTimer.expires_after(kKeepAliveIdleTimeout);
            auto raced = co_await (
                stream.async_read_some(asio::buffer(buffer), tuple) ||
                idleTimer.async_wait(tuple) ||
                shutdownSignal_.async_wait(tuple));
            if (raced.index() == 1) {
                keepGoing = false;  // idle too long
                break;
            }
            if (raced.index() == 2) {
                keepGoing = false;  // direct server shutdown
                break;
            }
            auto& [ec, n] = std::get<0>(raced);
            if (n > 0) {
                inbound.append(buffer.data(), n);
            }
            if (ec) {
                keepGoing = false;  // client closed or read error
                break;
            }
        }
        if (framed) {
            inbound.erase(0, consumed);  // keep any pipelined bytes for the next request
        }
    }

    asio::error_code ignore;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
}

template <typename Stream>
asio::awaitable<bool> EdgeServer::Impl::handleFramedRequest(
    Stream& stream,
    const detail::Http1ServerRequestParseState& parsed,
    std::string_view wireBody,
    std::string_view clientAddress) {
    const auto& request = parsed.request;
    Http1ResponseWriter<Stream> writer(stream, parsed.connectionPlan);

    EdgeRequest edgeRequest;
    edgeRequest.method = request.method();
    edgeRequest.knownMethod = request.knownMethod();
    edgeRequest.target = request.target();
    edgeRequest.host = request.header("host").value_or("");
    edgeRequest.headers = request.headers();
    edgeRequest.clientAddress = clientAddress;
    edgeRequest.keepAlive =
        parsed.connectionPlan.disposition() ==
        detail::Http1ConnectionDisposition::kReuse;

    // Read and decode the request body for methods that carry one, so the serve
    // core can forward it. `decodedBody` backs edgeRequest.body across the serve.
    std::string decodedBody;
    if (edgeRequest.knownMethod != HttpKnownMethod::kGet &&
        edgeRequest.knownMethod != HttpKnownMethod::kHead) {
        const auto& bodyPlan = parsed.bodyPlan;
        if (bodyPlan.knownLength() != nullptr) {
            edgeRequest.body = wireBody;
        } else if (bodyPlan.chunked() != nullptr) {
            ruvia::detail::Http1ChunkedBodyDecoder decoder(
                ProtocolByteLimit::limited(kMaxRequestBytes));
            std::string chunkBuffer(wireBody);
            bool decodeOk = true;
            for (;;) {
                const auto decoded = decoder.decode(chunkBuffer);
                if (decoded.failure() != nullptr) {
                    decodeOk = false;
                    break;
                }
                if (const auto* chunk = decoded.bodyChunk()) {
                    decodedBody.append(chunk->bytes());
                    chunkBuffer.erase(0, decoded.consumedBytes());
                    continue;
                }
                if (decoded.complete() != nullptr) {
                    break;
                }
                decodeOk = false;  // need-more is impossible: message is complete
                break;
            }
            if (!decodeOk) {
                const std::vector<std::pair<std::string, std::string>> noHeaders;
                co_await writer.respond(400, noHeaders, {}, "ERROR", std::nullopt, false, false);
                co_return false;
            }
            edgeRequest.body = decodedBody;
        }
    }

    const bool continueServing = co_await serveRequest(edgeRequest, writer);
    co_return continueServing && writer.connectionReusable();
}

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
    std::pmr::unordered_map<std::uint32_t, asio::steady_timer*> drainWaiters(
        memory_.resource());
    bool shuttingDown = false;
    int activeHandlers = 0;
    // Per-stream cancellation signals for the detached response handlers. On
    // teardown beginShutdown() emits terminal cancellation on each, so a handler
    // parked somewhere it does not otherwise observe the shutdown -- the request-
    // coalescing wait, or an in-flight origin fetch -- is released and unwinds.
    // Node-based storage: cancellation_signal is not movable, and the slot a
    // spawned handler binds must stay valid until that handler completes.
    std::pmr::unordered_map<std::uint32_t, asio::cancellation_signal> handlerCancels(
        memory_.resource());
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
    auto serveStream =
        [this, &shared, resource = &resource, clientAddress = std::string_view(clientAddress)](
            std::uint32_t streamId, detail::Http2StreamState& streamState,
            std::pmr::string requestBody) -> asio::awaitable<void> {
        HttpRequest httpRequest = detail::HttpRequestAccess::make();
        const auto buildResult =
            detail::Http2RequestBuilder::build(streamState, httpRequest, resource, requestBody);
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
        if (edgeRequest.knownMethod != HttpKnownMethod::kGet &&
            edgeRequest.knownMethod != HttpKnownMethod::kHead && !requestBody.empty()) {
            edgeRequest.body = std::string_view(requestBody);
        }

        Http2ResponseWriter writer(shared, streamId, edgeRequest.knownMethod, resource);
        (void)co_await serveRequest(edgeRequest, writer);
        // If the serve core returned without completing the response (client gone
        // mid-stream), reset the stream so it does not dangle.
        if (!writer.ended()) {
            auto* s = shared.connection.stream(streamId);
            if (s != nullptr && !s->isAborted()) {
                (void)shared.connection.submitReset(streamId,
                                                    detail::Http2ErrorCode::kInternalError);
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
                auto [ec, n] = co_await asio::async_write(
                    stream, asio::buffer(out.data(), out.size()), tuple);
                (void)n;
                if (ec) {
                    beginShutdown();
                    co_return;
                }
            }
            if (connection.connectionError().has_value() ||
                (shuttingDown && activeHandlers == 0)) {
                co_return;
            }
            writeWake.expires_at((std::chrono::steady_clock::time_point::max)());
            co_await writeWake.async_wait(tuple);
        }
    };

    // Reader loop: read, feed, dispatch each completed request to its own handler
    // coroutine so a slow origin on one stream never blocks the others.
    std::array<char, 16384> readBuffer;
    std::pmr::unordered_map<std::uint32_t, std::pmr::string> requestBodies(
        memory_.resource());

    auto reader = [&]() -> asio::awaitable<void> {
        for (;;) {
            std::size_t readSize = 0;
            asio::error_code readError;
            auto raced = co_await (
                stream.async_read_some(asio::buffer(readBuffer), tuple) ||
                shutdownSignal_.async_wait(tuple));
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
            std::array<std::uint32_t,
                       detail::Http2LocalSettings::kMaxConcurrentStreams>
                copiedBodyStreams{};
            std::size_t copiedBodyStreamCount = 0;
            const auto markBodyCopied = [&](std::uint32_t streamId) {
                const auto copied = std::span(copiedBodyStreams)
                                        .first(copiedBodyStreamCount);
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
                auto copied = std::span(copiedBodyStreams)
                                  .first(copiedBodyStreamCount);
                const auto found = std::ranges::find(copied, streamId);
                if (found == copied.end()) {
                    return;
                }
                --copiedBodyStreamCount;
                *found = copiedBodyStreams[copiedBodyStreamCount];
            };
            const auto resetBodyStream = [&](std::uint32_t streamId,
                                             detail::Http2ErrorCode error) {
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
                    const auto* knownLength = streamState == nullptr
                        ? nullptr
                        : streamState->remoteContent().allowedKnownLength();
                    if (knownLength != nullptr &&
                        knownLength->declaredLength() > kMaxRequestBytes) {
                        resetBodyStream(
                            streamId, detail::Http2ErrorCode::kCancel);
                    } else {
                        requestBodies.try_emplace(streamId);
                    }
                } else if (const auto* chunk = event->messageBodyChunk()) {
                    const auto streamId = chunk->streamId();
                    const auto body = requestBodies.find(streamId);
                    if (body == requestBodies.end()) {
                        resetBodyStream(
                            streamId,
                            detail::Http2ErrorCode::kInternalError);
                        continue;
                    }
                    if (chunk->bytes().size() >
                        kMaxRequestBytes - body->second.size()) {
                        resetBodyStream(
                            streamId, detail::Http2ErrorCode::kCancel);
                        continue;
                    }
                    body->second.append(chunk->bytes());
                    if (!markBodyCopied(streamId)) {
                        resetBodyStream(
                            streamId,
                            detail::Http2ErrorCode::kInternalError);
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
                        throw std::logic_error(
                            "duplicate HTTP/2 handler for one stream");
                    }
                    bool handlerRegistered = false;
                    try {
                        // Pin so the stream's request/response storage outlives
                        // the detached handler; unpin on its completion.
                        connection.pinStream(streamId);
                        ++activeHandlers;
                        handlerRegistered = true;
                        asio::co_spawn(
                            executor, serveStream(streamId, *streamState, std::move(body)),
                            asio::bind_cancellation_slot(
                                cancelIt->second.slot(),
                                [&, streamId](std::exception_ptr failure) {
                                    if (failure != nullptr && !shuttingDown) {
                                        auto* failedStream = connection.stream(streamId);
                                        if (failedStream != nullptr &&
                                            !failedStream->isAborted()) {
                                            // A handler that unwinds without a
                                            // terminal response still owes the
                                            // peer a stream terminal state.
                                            (void)connection.submitReset(
                                                streamId,
                                                detail::Http2ErrorCode::kInternalError);
                                        }
                                    }
                                    connection.unpinStream(streamId);
                                    drainWaiters.erase(streamId);
                                    handlerCancels.erase(streamId);
                                    --activeHandlers;
                                    writeWake.cancel();  // let the writer re-check its exit
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
                    if (const auto it = drainWaiters.find(closed->streamId());
                        it != drainWaiters.end()) {
                        it->second->cancel();
                    }
                    requestBodies.erase(closed->streamId());
                }
            }

            for (std::size_t index = 0;
                 index < copiedBodyStreamCount;
                 ++index) {
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

    try {
        co_await (reader() && writer());
    } catch (...) {
        // A synchronous throw (e.g. bad_alloc from the writer's output buffer or
        // the reader's body accumulation) can escape the group while a detached
        // handler is still awaiting its origin fetch. Signal teardown so every
        // handler unwinds, then fall through to join them before the locals they
        // captured by reference are destroyed.
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
}

}  // namespace ruvia::edge
