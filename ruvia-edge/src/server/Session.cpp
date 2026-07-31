#include "ruvia/edge/detail/server/ServerImpl.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include "ruvia/edge/detail/http1/Http1ResponseWriter.h"
#include "ruvia/edge/detail/http2/Http2Session.h"
#include "ruvia/edge/detail/server/SessionLimits.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace ruvia::edge {

asio::awaitable<void> EdgeServer::Impl::acceptLoop() {
    // Pause length after an accept that failed for a reason retrying cannot fix
    // immediately: descriptor exhaustion, or a session that could not be
    // started because memory is gone. Retrying either at full speed burns the
    // worker on a failure loop and starves the sessions that would release the
    // very resource being waited for.
    static constexpr auto kRetryDelay = std::chrono::milliseconds(50);
    asio::steady_timer retryTimer(ioContext_);
    const auto pause = [&retryTimer]() -> asio::awaitable<void> {
        retryTimer.expires_after(kRetryDelay);
        co_await retryTimer.async_wait(asio::as_tuple(asio::use_awaitable));
    };

    for (;;) {
        auto [ec, socket] = co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            // Cancelled or closed listener: this is shutdown, not a failure.
            if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor || ec == asio::error::invalid_argument) {
                break;
            }
            // Everything else is transient (EMFILE/ENFILE, ECONNABORTED,
            // ENOBUFS). Under descriptor exhaustion the failing accept stays
            // instantly ready, so continuing without a pause would spin at
            // 100% CPU for as long as the condition lasts.
            co_await pause();
            if (shutdownRequestedOnWorker_) {
                break;
            }
            continue;
        }

        // Over budget: accept and close immediately rather than leaving the
        // connection queued in the backlog, so the peer learns now and the
        // listener queue keeps draining.
        if (maxConnections_.has_value() && activeConnections_.load(std::memory_order_relaxed) >= *maxConnections_) {
            asio::error_code ignore;
            socket.close(ignore);
            connectionsRefused_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        auto tlsContext = loadTlsContext();
        // Spawning is the one part of accepting that can throw. It must not end
        // this loop: the listener would stay open while the node silently
        // stopped serving. Report the failure, drop the connection with it (the
        // unspawned coroutine frame closes the socket), pause, and keep going.
        try {
            ConnectionLease lease(activeConnections_);
            if (tlsContext != nullptr) {
                spawnTracked(handleTlsSession(std::move(socket), std::move(tlsContext), std::move(lease)), EdgeTaskKind::kSession);
            } else {
                spawnTracked(handleSession(std::move(socket), std::move(lease)), EdgeTaskKind::kSession);
            }
            continue;
        } catch (...) {
            reportFailure(EdgeTaskKind::kAcceptLoop, std::current_exception());
        }
        co_await pause();
        if (shutdownRequestedOnWorker_) {
            break;
        }
    }
}

asio::awaitable<void> EdgeServer::Impl::handleTlsSession(asio::ip::tcp::socket socket, TlsContextPtr context, ConnectionLease lease) {
    // The accept loop pins the context for this session's lifetime; a runtime
    // rotation only affects connections accepted afterward.
    asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), *context);
    auto [ec] = co_await stream.async_handshake(asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
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
        // The lease stays in this frame: it outlives the awaited session and is
        // released when this coroutine ends, whichever way that happens.
        co_await handleHttp2Session(std::move(stream), std::move(clientAddress));
        co_return;
    }
    co_await handleSession(std::move(stream), std::move(lease));
}

template <typename Stream>
asio::awaitable<void> EdgeServer::Impl::handleSession(Stream stream, [[maybe_unused]] ConnectionLease lease) {
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
                co_await writeHttp1StatusResponse(stream, parseState.connectionPlan, memory_.resource(), protocolError.status().value());
                keepGoing = false;
                break;
            }
            const auto* needBody = parseState.needRequestBody();
            const auto* ready = parseState.messageReady();
            const bool exceedsEdgeRequestLimit = (needBody != nullptr && needBody->requiredTotalBytes().has_value() && *needBody->requiredTotalBytes() > kMaxRequestBytes) || (ready != nullptr && ready->messageBytes() > kMaxRequestBytes);
            if (exceedsEdgeRequestLimit) {
                // Apply the edge product's tighter buffered-request policy to
                // parser metadata before waiting for or dispatching the body.
                // Checking only inbound.size() after messageReady lets the read
                // that completes an oversized request jump over the limit.
                co_await writeHttp1StatusResponse(stream, parseState.connectionPlan, memory_.resource(), 413);
                keepGoing = false;
                break;
            }
            if (ready != nullptr) {
                consumed = ready->messageBytes();
                const auto wireBody = std::string_view(inbound).substr(ready->headerBytes(), ready->messageBytes() - ready->headerBytes());
                keepGoing = co_await handleFramedRequest(stream, parseState, wireBody, clientAddress);
                framed = true;
                break;
            }
            if (inbound.size() > kMaxRequestBytes) {
                co_await writeHttp1StatusResponse(stream, parseState.connectionPlan, memory_.resource(), 413);
                keepGoing = false;
                break;
            }
            idleTimer.expires_after(kKeepAliveIdleTimeout);
            auto raced = co_await (stream.async_read_some(asio::buffer(buffer), tuple) || idleTimer.async_wait(tuple) || shutdownSignal_.async_wait(tuple));
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
asio::awaitable<bool> EdgeServer::Impl::handleFramedRequest(Stream& stream, const detail::Http1ServerRequestParseState& parsed, std::string_view wireBody, std::string_view clientAddress) {
    const auto& request = parsed.request;
    Http1ResponseWriter<Stream> writer(stream, parsed, memory_.resource());

    EdgeRequest edgeRequest;
    edgeRequest.method = request.method();
    edgeRequest.knownMethod = request.knownMethod();
    edgeRequest.target = request.target();
    edgeRequest.host = request.header("host").value_or("");
    edgeRequest.headers = request.headers();
    edgeRequest.clientAddress = clientAddress;
    edgeRequest.keepAlive = parsed.connectionPlan.disposition() == detail::Http1ConnectionDisposition::kReuse;

    // Read and decode the request body for methods that carry one, so the serve
    // core can forward it. `decodedBody` backs edgeRequest.body across the serve.
    std::string decodedBody;
    if (edgeRequest.knownMethod != HttpKnownMethod::kGet && edgeRequest.knownMethod != HttpKnownMethod::kHead) {
        const auto& bodyPlan = parsed.bodyPlan;
        if (bodyPlan.knownLength() != nullptr) {
            edgeRequest.body = wireBody;
        } else if (bodyPlan.chunked() != nullptr) {
            ruvia::detail::Http1ChunkedBodyDecoder decoder(ProtocolByteLimit::limited(kMaxRequestBytes));
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
                co_await respondStatusOnly(writer, 400, "ERROR", ResponseReusePolicy::kClose);
                co_return false;
            }
            edgeRequest.body = decodedBody;
        }
    }

    const bool continueServing = co_await serveRequest(edgeRequest, writer);
    co_return continueServing&& writer.connectionReusable();
}

}  // namespace ruvia::edge
