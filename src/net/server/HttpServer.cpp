#include "HttpServer.h"

#include <asio/ssl.hpp>
#include <array>
#include <type_traits>

#include "ConnectionScanner.h"
#include "HttpConnectionState.h"
#include "../body/HttpRequestBody.h"
#include "HttpResponseWriter.h"
#include "../ws/HttpWebSocketConnection.h"
#include "../ws/HttpWebSocketHandshake.h"
#include "../ws/HttpWebSocketUtils.h"
#include "../../http/HttpCors.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpParser.h"
#include "../../runtime/AsioAwait.h"
#include "../../router/RouterInternal.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;
using TcpSocket = asio::ip::tcp::socket;

namespace {

constexpr std::size_t kRequestArenaStackBytes = 4 * 1024;
void closeSocket(TcpSocket& socket) noexcept {
    std::error_code ignored;
    socket.cancel(ignored);
    socket.shutdown(TcpSocket::shutdown_both, ignored);
    socket.close(ignored);
}

void configureAcceptedSocket(TcpSocket& socket) noexcept {
    std::error_code ignored;
    socket.set_option(asio::ip::tcp::no_delay(true), ignored);
}

bool responseWantsClose(const HttpResponse& response) noexcept {
    return detail::httpHasToken(response.header(HttpResponse::kKnownHeaderConnection), "close");
}

bool contentLengthExceedsLimit(std::size_t contentLength, std::size_t limit) noexcept {
    return limit != 0 && contentLength > limit;
}

bool shouldKeepAlive(const HttpParseResult& parsed) noexcept {
    if (parsed.flags.connectionClose) {
        return false;
    }
    if (parsed.flags.connectionKeepAlive) {
        return true;
    }
    return parsed.request.httpVersion() == "HTTP/1.1";
}

bool wantsContinue(const HttpParseResult& parsed) noexcept {
    return parsed.flags.expectContinue;
}

class ConnectionCountGuard final {
public:
    explicit ConnectionCountGuard(std::size_t& count) noexcept
        : count_(&count) {}

    ConnectionCountGuard(const ConnectionCountGuard&) = delete;
    ConnectionCountGuard& operator=(const ConnectionCountGuard&) = delete;

    ~ConnectionCountGuard() {
        if (count_ != nullptr && *count_ > 0) {
            --*count_;
        }
    }

private:
    std::size_t* count_;
};

// Returns a connection's borrowed work set to the per-worker pool on scope
// exit, covering every way a session ends (keep-alive close, read error, or any
// co_return). Tracks the work-set pointer variable by reference so the explicit
// idle-gap release (which nulls it) is not double-released.
class WorkSetReturn final {
public:
    WorkSetReturn(ConnectionWorkSetPool& pool, ConnectionWorkSet*& workSet) noexcept
        : pool_(&pool), workSet_(&workSet) {}

    WorkSetReturn(const WorkSetReturn&) = delete;
    WorkSetReturn& operator=(const WorkSetReturn&) = delete;

    ~WorkSetReturn() {
        if (*workSet_ != nullptr) {
            pool_->release(*workSet_);
        }
    }

private:
    ConnectionWorkSetPool* pool_;
    ConnectionWorkSet** workSet_;
};

}  // namespace

std::optional<HttpResponse> HttpServer::tryDocumentRootResponse(
    const HttpRequest& request,
    RequestMemory& memory) const {
    const auto* const root = options_.documentRoot.root;
    if (root == nullptr) {
        return std::nullopt;
    }
    if (request.method() != HttpMethod::kGet) {
        return std::nullopt;
    }

    auto relative = request.path();
    if (!relative.empty() && relative.front() == '/') {
        relative.remove_prefix(1);
    }

    Context context(memory, request);
    try {
        return context.staticFile(*root, relative);
    } catch (const HttpError&) {
        return std::nullopt;
    }
}

Task<void> HttpServer::acceptLoop() {
    asio::steady_timer retryTimer(ioContext_);
    for (;;) {
        auto [ec, socket] = co_await asyncResult<TcpSocket>([this](auto handler) mutable {
            acceptor_.async_accept(std::move(handler));
        });

        if (ec) {
            // Fatal: acceptor was cancelled (stop()) or closed. Exit cleanly.
            if (ec == asio::error::operation_aborted ||
                ec == asio::error::bad_descriptor ||
                ec == asio::error::invalid_argument) {
                co_return;
            }
            // Transient: EMFILE/ENFILE (fd exhaustion), ECONNABORTED (client
            // gave up before accept), EINTR, ENOBUFS, ENOMEM, etc. A single
            // bad accept must not stop the worker forever; back off briefly
            // and keep listening. stop() interrupts the wait via cancel().
            retryTimer.expires_after(std::chrono::milliseconds(50));
            const auto waitEc = co_await asyncError([&retryTimer](auto handler) mutable {
                retryTimer.async_wait(std::move(handler));
            });
            if (waitEc || !started_.load(std::memory_order_relaxed)) {
                co_return;
            }
            continue;
        }

        if (!started_.load(std::memory_order_relaxed)) {
            closeSocket(socket);
            co_return;
        }
        configureAcceptedSocket(socket);

        if (options_.maxConnections > 0 && activeConnectionCount_ >= options_.maxConnections) {
            if (options_.tls.enabled) {
                closeSocket(socket);
                continue;
            }
            std::array<std::byte, kRequestArenaStackBytes> limitArenaBuffer;
            std::optional<RequestMemory> limitMemoryStorage;
            if (memory_.requestInitialBufferBytes() <= limitArenaBuffer.size()) {
                limitMemoryStorage.emplace(
                    memory_,
                    std::span<std::byte>(limitArenaBuffer.data(), memory_.requestInitialBufferBytes()));
            } else {
                limitMemoryStorage.emplace(memory_);
            }
            auto& limitMemory = *limitMemoryStorage;
            auto response = makeErrorResponse(
                limitMemory.resource(),
                HttpErrorInfo{
                    .statusCode = 503,
                    .message = "too many active connections"},
                true);
            std::error_code writeEc;
            co_await writeResponse(socket, memory_, nullptr, nullptr, response, false, writeEc);
            closeSocket(socket);
            continue;
        }

        ++activeConnectionCount_;

        asio::co_spawn(
            ioContext_,
            taskAsAwaitable(handleSession(std::move(socket))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    }
}

Task<void> HttpServer::handleSession(TcpSocket socket) {
    try {
        ConnectionCountGuard connectionCount(activeConnectionCount_);
        if (options_.tls.enabled) {
            ConnectionScanner::Entry handshakeEntry;
            {
                ConnectionScanner::Guard handshakeGuard(connectionScanner_.get(), handshakeEntry, socket);
                handshakeEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
                asio::ssl::stream<TcpSocket&> tlsStream(socket, *tlsContext_);
                const auto ec = co_await asyncError([&tlsStream](auto handler) mutable {
                    tlsStream.async_handshake(asio::ssl::stream_base::server, std::move(handler));
                });
                if (ec) {
                    closeSocket(socket);
                    co_return;
                }
                handshakeEntry.touch();
                co_await handleStreamSession(tlsStream, socket);
            }
            closeSocket(socket);
            co_return;
        }
        co_await handleStreamSession(socket, socket);
    } catch (...) {
        // Last-resort safety net: any exception that escapes the session
        // body (including bad_alloc, error-handler failures, or framework
        // bugs) must not propagate into asio::detached, which terminates.
        // Socket state may be partially written or completely fine; we
        // cannot safely emit anything new, so just drop the connection.
        closeSocket(socket);
    }
}

template <typename Stream>
Task<void> HttpServer::handleStreamSession(Stream& stream, TcpSocket& socket) {
    // Resident connection identity (held for the whole connection): the scanner
    // entry, keep-alive counters, the remote address, and the count of buffered
    // bytes. The heavy per-request working set (read buffer, request arena,
    // parse result, response head, file chunk) is borrowed from a per-worker
    // pool only while the connection is actively serving and returned the moment
    // it goes idle, so an idle keep-alive connection holds none of it.
    ConnectionScanner::Entry scannerEntry;
    ConnectionScanner::Guard scannerGuard(connectionScanner_.get(), scannerEntry, socket);
    const auto& routes = routes_;
    std::pmr::string remoteAddress(memory_.allocator<char>());
    std::error_code remoteEc;
    const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
    if (!remoteEc) {
        remoteAddress = remoteEndpoint.address().to_string();
    }
    std::size_t requestCount = 0;
    std::size_t usedBytes = 0;
    ConnectionWorkSet* workSet = nullptr;
    WorkSetReturn workSetReturn(*workSetPool_, workSet);

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
            if (usedBytes == 0) {
                std::error_code availabilityEc;
                const auto pendingBytes = socket.available(availabilityEc);
                if (!availabilityEc && pendingBytes == 0) {
                    if (workSet != nullptr) {
                        workSetPool_->release(workSet);
                        workSet = nullptr;
                    }
                    // Wait bufferless for the next request under the same phase
                    // the buffered gap read used (kReadingHeader), so the
                    // headerTimeout/idleTimeout bound on an idle keep-alive
                    // connection is unchanged.
                    scannerEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
                    const auto waitEc = co_await asyncError([&socket](auto handler) mutable {
                        socket.async_wait(TcpSocket::wait_read, std::move(handler));
                    });
                    if (waitEc || !started_.load(std::memory_order_relaxed)) {
                        co_return;
                    }
                }
            }
        }
        if (workSet == nullptr) {
            workSet = workSetPool_->acquire();
        }
        auto& readBuffer = workSet->readBuffer;
        auto& parser = workSet->parser;
        auto& parsed = workSet->parsed;
        auto& responseHead = workSet->responseHead;
        auto& fileChunk = workSet->fileChunk;
        auto& routeResolution = workSet->routeResolution;

        std::optional<RequestMemory> requestMemoryStorage;
        if (memory_.requestInitialBufferBytes() <= sizeof(workSet->arenaBlock)) {
            requestMemoryStorage.emplace(
                memory_,
                std::span<std::byte>(workSet->arenaBlock, memory_.requestInitialBufferBytes()));
        } else {
            requestMemoryStorage.emplace(memory_);
        }
        auto& requestMemory = *requestMemoryStorage;
        HttpResponse response(requestMemory.resource());
        bool keepAlive = false;
        bool closeAfterWrite = false;
        bool responseStreamDispatched = false;
        bool bufferAlreadyCompacted = false;
        std::size_t consumedBytes = 0;
        std::size_t headerSearchOffset = 0;
        for (;;) {
            const auto bufferView = std::string_view(readBuffer.data(), usedBytes);
            parser.parseHeaders(bufferView, parsed, headerSearchOffset);
            if (parsed.status == HttpParseStatus::kComplete) {
                parsed.request.setResource(requestMemory.resource());
                parsed.request.setRemoteAddress(remoteAddress);
                // Reset phase so headerTimeout stops counting against dispatch
                // time. Body readers will set kReadingBody on their own; the
                // streaming/websocket paths set their own phases below; the
                // buffered write path sets kWriting before responding. Until
                // one of those transitions, idleTimeout governs as the
                // deadman switch for hung handlers.
                scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
                routeResolution = routes.resolve(parsed.request);
                if (!routeResolution.found()) {
                    consumedBytes = parsed.headerBytes;
                    if (contentLengthExceedsLimit(parsed.contentLength, options_.maxBufferedBodyBytes)) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo{.statusCode = 413, .message = "request body is too large"},
                            true
                            ,
                            &databases_,
                            &redis_
                        );
                        response.setHeader("Connection", "close");
                        closeAfterWrite = true;
                        break;
                    }
                    if (auto documentResponse = tryDocumentRootResponse(parsed.request, requestMemory)) {
                        response = std::move(*documentResponse);
                        keepAlive = shouldKeepAlive(parsed) &&
                            parsed.contentLength == 0 &&
                            !parsed.chunked;
                        ++requestCount;
                        if (options_.maxRequestsPerConnection > 0 &&
                            requestCount >= options_.maxRequestsPerConnection) {
                            keepAlive = false;
                        }
                        if (!keepAlive) {
                            response.setHeader("Connection", "close");
                        }
                        scannerEntry.touch();
                        break;
                    }
                    response = co_await routes.dispatch(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        &databases_,
                        &redis_);
                    response.setHeader("Connection", "close");
                    closeAfterWrite = true;
                    break;
                }

                const auto maxRequestBodyBytes = routeResolution.bodyMode == RequestBodyMode::kStream
                    ? options_.maxStreamBodyBytes
                    : options_.maxBufferedBodyBytes;
                if (contentLengthExceedsLimit(parsed.contentLength, maxRequestBodyBytes)) {
                    consumedBytes = parsed.headerBytes;
                    response = co_await routes.handleError(
                        parsed.request,
                        requestMemory,
                        HttpErrorInfo{.statusCode = 413, .message = "request body is too large"},
                        true
                        ,
                        &databases_,
                        &redis_
                    );
                    response.setHeader("Connection", "close");
                    closeAfterWrite = true;
                    break;
                }

                if (routeResolution.route->responseMode == ResponseBodyMode::kWebSocket) {
                    consumedBytes = parsed.headerBytes;
                    if (!isValidWebSocketRequest(parsed.request, parsed.flags) || parsed.contentLength != 0 || parsed.chunked) {
                        response = co_await routes.handleError(
                            parsed.request,
                            requestMemory,
                            HttpErrorInfo{.statusCode = 400, .message = "invalid websocket upgrade"},
                            true
                            ,
                            &databases_,
                            &redis_
                        );
                        response.setHeader("Connection", "close");
                        closeAfterWrite = true;
                        break;
                    }
                    if (!(co_await writeWebSocketHandshake(
                            stream,
                            parsed.request,
                            parsed.flags,
                            routeResolution.route->webSocketSubprotocols,
                            requestMemory.resource()))) {
                        co_return;
                    }
                    const auto pendingFrames = std::string_view(
                        readBuffer.data() + parsed.headerBytes,
                        usedBytes - parsed.headerBytes);
                    WebSocketConnection<Stream> webSocketConnection(
                        stream,
                        memory_.resource(),
                        scannerEntry,
                        routeResolution.route->webSocketHeartbeat,
                        options_.maxWebSocketMessageBytes,
                        pendingFrames);
                    WebSocket webSocket(
                        &webSocketConnection,
                        &WebSocketConnection<Stream>::readThunk,
                        &WebSocketConnection<Stream>::writeThunk,
                        &WebSocketConnection<Stream>::closeThunk);
                    std::exception_ptr webSocketException;
                    try {
                        scannerEntry.setPhase(ConnectionScanner::Phase::kWebSocket);
                        (void)co_await routes.dispatchWebSocket(
                            parsed.request,
                            routeResolution,
                            requestMemory,
                            webSocket
                            ,
                            &databases_,
                            &redis_
                        );
                    } catch (...) {
                        webSocketException = std::current_exception();
                    }
                    if (webSocketException != nullptr) {
                        // Send 1011 (server internal error) so the peer learns
                        // the connection died from a handler fault, not just a
                        // raw TCP drop. close() itself may fail (socket gone,
                        // concurrent write active); swallow that too.
                        try {
                            co_await webSocketConnection.close(1011, "internal server error");
                        } catch (...) {
                        }
                    }
                    co_await webSocketConnection.detachAndDrainBackgroundWrites();
                    co_return;
                }

                if (routeResolution.route->responseMode != ResponseBodyMode::kBuffered) {
                    consumedBytes = parsed.headerBytes;
                    keepAlive = shouldKeepAlive(parsed) && parsed.contentLength == 0 && !parsed.chunked;
                    using ResponseSink = ResponseStreamSink<Stream, std::remove_reference_t<decltype(scannerEntry)>>;
                    ResponseSink responseSink(
                        stream,
                        memory_,
                        responseHead,
                        scannerEntry,
                        routeResolution.route->responseMode);
                    ResponseStreamWriter responseStream(
                        &responseSink,
                        &ResponseSink::writeThunk,
                        &ResponseSink::endThunk,
                        &ResponseSink::bindContextThunk,
                        &ResponseSink::scratchThunk);
                    std::exception_ptr exception;
                    bool streamHandled = false;
                    try {
                        scannerEntry.setPhase(ConnectionScanner::Phase::kWriting);
                        auto result = co_await routes.dispatchResponseStream(
                            parsed.request,
                            routeResolution,
                            requestMemory,
                            responseStream,
                            &databases_,
                            &redis_,
                            nullptr,
                            nullptr);
                        streamHandled = result.streamHandled;
                        if (streamHandled || responseSink.committed()) {
                            co_await responseStream.end();
                        } else {
                            response = std::move(result.response);
                        }
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    if (exception != nullptr) {
                        if (responseSink.committed()) {
                            co_return;
                        }
                        response = co_await routes.handleException(
                            parsed.request,
                            requestMemory,
                            exception,
                            true
                            ,
                            &databases_,
                            &redis_
                        );
                        keepAlive = false;
                    } else {
                        if (!streamHandled && !responseSink.committed()) {
                            if (responseWantsClose(response)) {
                                keepAlive = false;
                            }
                            ++requestCount;
                            if (options_.maxRequestsPerConnection > 0 &&
                                requestCount >= options_.maxRequestsPerConnection) {
                                keepAlive = false;
                            }
                            if (!keepAlive) {
                                response.setHeader("Connection", "close");
                            }
                            scannerEntry.touch();
                            break;
                        }
                        ++requestCount;
                        if (options_.maxRequestsPerConnection > 0 &&
                            requestCount >= options_.maxRequestsPerConnection) {
                            keepAlive = false;
                        }
                        if (!keepAlive) {
                            co_return;
                        }
                        responseStreamDispatched = true;
                        bufferAlreadyCompacted = false;
                        break;
                    }
                    scannerEntry.touch();
                    break;
                }
                if (routeResolution.bodyMode == RequestBodyMode::kStream) {
                    consumedBytes = parsed.headerBytes;
                    keepAlive = shouldKeepAlive(parsed);
                    const auto bodyAndPipeline = std::string_view(
                        readBuffer.data() + parsed.headerBytes,
                        usedBytes - parsed.headerBytes);
                    std::exception_ptr exception;
                    std::optional<StreamBodyReader<Stream>> streamReader;
                    std::optional<BodyReader> bodyReader;
                    try {
                        streamReader.emplace(
                            stream,
                            memory_.allocator<char>(),
                            bodyAndPipeline,
                            parsed.contentLength,
                            parsed.chunked,
                            parsed.transferCodings,
                            options_.maxStreamBodyBytes,
                            scannerEntry,
                            (parsed.contentLength > 0 || parsed.chunked) && wantsContinue(parsed));
                        bodyReader.emplace(&*streamReader, &StreamBodyReader<Stream>::readThunk);
                        response = co_await routes.dispatch(
                            parsed.request,
                            routeResolution,
                            requestMemory,
                            &databases_,
                            &redis_,
                            &*bodyReader,
                            nullptr);
                    } catch (...) {
                        exception = std::current_exception();
                    }
                    if (exception != nullptr) {
                        response = co_await routes.handleException(
                            parsed.request,
                            requestMemory,
                            exception,
                            true,
                            &databases_,
                            &redis_,
                            bodyReader ? &*bodyReader : nullptr,
                            nullptr);
                        response.materializeBody();
                        keepAlive = false;
                    } else {
                        if (responseWantsClose(response) || !streamReader->finished()) {
                            keepAlive = false;
                        }
                        ++requestCount;
                        if (options_.maxRequestsPerConnection > 0 &&
                            requestCount >= options_.maxRequestsPerConnection) {
                            keepAlive = false;
                        }
                        // Fix borrowed response views before restoring pipeline bytes:
                        // bodyReader chunks may point into streamReader/readBuffer storage.
                        response.materializeBody();
                        if (keepAlive) {
                            streamReader->restorePipeline(readBuffer, usedBytes);
                            consumedBytes = 0;
                            bufferAlreadyCompacted = true;
                        }
                        if (!keepAlive) {
                            response.setHeader("Connection", "close");
                        }
                    }
                    scannerEntry.touch();
                    break;
                }

                consumedBytes = parsed.headerBytes;
                keepAlive = shouldKeepAlive(parsed);
                const auto bodyAndPipeline = std::string_view(
                    readBuffer.data() + parsed.headerBytes,
                    usedBytes - parsed.headerBytes);
                std::exception_ptr exception;
                std::optional<LazyBufferedBody<Stream>> lazyBody;
                std::optional<RequestBodyLoader> bodyLoader;
                try {
                    lazyBody.emplace(
                        stream,
                        memory_.allocator<char>(),
                        requestMemory.resource(),
                        bodyAndPipeline,
                        parsed.contentLength,
                        parsed.chunked,
                        parsed.transferCodings,
                        options_.maxBufferedBodyBytes,
                        scannerEntry,
                        (parsed.contentLength > 0 || parsed.chunked) && wantsContinue(parsed));
                    bodyLoader.emplace(&*lazyBody, &LazyBufferedBody<Stream>::readAllThunk, &LazyBufferedBody<Stream>::discardThunk);
                    response = co_await routes.dispatch(
                        parsed.request,
                        routeResolution,
                        requestMemory,
                        &databases_,
                        &redis_,
                        nullptr,
                        &*bodyLoader);
                } catch (...) {
                    exception = std::current_exception();
                }
                if (exception != nullptr) {
                    response = co_await routes.handleException(
                        parsed.request,
                        requestMemory,
                        exception,
                        true
                        ,
                        &databases_,
                        &redis_
                    );
                    response.materializeBody();
                    keepAlive = false;
                } else {
                    if (responseWantsClose(response) || !lazyBody->consumed()) {
                        keepAlive = false;
                    }
                    ++requestCount;
                    if (options_.maxRequestsPerConnection > 0 &&
                        requestCount >= options_.maxRequestsPerConnection) {
                        keepAlive = false;
                    }
                    // Fix borrowed response views before lazyBody is destroyed or
                    // pipeline bytes are restored into the connection read buffer.
                    response.materializeBody();
                    if (keepAlive) {
                        lazyBody->restorePipeline(readBuffer, usedBytes);
                        consumedBytes = 0;
                        bufferAlreadyCompacted = true;
                    }
                    if (!keepAlive) {
                        response.setHeader("Connection", "close");
                    }
                }
                scannerEntry.touch();
                break;
            }

            if (parsed.status == HttpParseStatus::kError) {
                const auto error = parsed.error;
                parsed.request.setResource(requestMemory.resource());
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo{.statusCode = httpParseErrorStatus(error), .message = httpParseErrorMessage(error)},
                    true
                    ,
                    &databases_,
                    &redis_
                );
                closeAfterWrite = true;
                break;
            }

            headerSearchOffset = usedBytes > 3 ? usedBytes - 3 : 0;

            scannerEntry.setPhase(ConnectionScanner::Phase::kReadingHeader);
            growReadBuffer(readBuffer, usedBytes, parsed);
            if (usedBytes == readBuffer.size()) {
                constexpr auto error = HttpParseError::kHeaderTooLarge;
                parsed.request.setResource(requestMemory.resource());
                response = co_await routes.handleError(
                    parsed.request,
                    requestMemory,
                    HttpErrorInfo{.statusCode = httpParseErrorStatus(error), .message = httpParseErrorMessage(error)},
                    true
                    ,
                    &databases_,
                    &redis_
                );
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
            applyCorsHeaders(parsed.request, response, options_.cors);
            const bool skipResponseBody = parsed.request.method() == HttpMethod::kHead;
            (void)compressResponseBodyIfAccepted(parsed.flags, response, options_.compression, skipResponseBody);
            co_await writeResponse(
                stream,
                memory_,
                &responseHead,
                &fileChunk,
                response,
                skipResponseBody,
                ec);
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
            if (ec || closeAfterWrite || !keepAlive || !started_.load(std::memory_order_relaxed)) {
                co_return;
            }
        } else {
            scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
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

}  // namespace ruvia::detail
