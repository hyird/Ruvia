#include "HttpServer.h"

#include <asio/ssl.hpp>
#include <charconv>
#include <cstring>
#include <openssl/ssl.h>
#include <stdexcept>

#include "ConnectionScanner.h"
#include "HttpRequestBody.h"
#include "HttpResponseWriter.h"
#include "HttpWebSocketConnection.h"
#include "HttpWebSocketUtils.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HeaderUtils.h"
#include "ruvia/http/HttpParser.h"
#include "../AsioAwait.h"
#include "../router/RouterInternal.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;
using TcpSocket = asio::ip::tcp::socket;

namespace {

constexpr std::size_t kInitialReadBufferBytes = 8 * 1024;
constexpr std::size_t kRequestArenaStackBytes = 4 * 1024;
constexpr std::size_t kReadBufferShrinkCapacityBytes = 64 * 1024;
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

void addVaryToken(HttpResponse& response, std::string_view token) {
    const auto vary = response.header(HttpResponse::kKnownHeaderVary);
    if (vary.empty()) {
        response.setHeader("Vary", token);
        return;
    }
    if (detail::httpHasToken(vary, token)) {
        return;
    }
    std::pmr::string updated(response.resource());
    updated.append(vary);
    updated.append(", ");
    updated.append(token.data(), token.size());
    response.setHeader("Vary", updated);
}

void setHeaderIfMissing(
    HttpResponse& response,
    HttpResponse::KnownHeaderBit bit,
    std::string_view name,
    std::string_view value) {
    if (!response.hasKnownHeader(bit)) {
        response.setHeader(name, value);
    }
}

void setCorsMaxAge(HttpResponse& response, std::chrono::seconds maxAge) {
    if (maxAge.count() <= 0 || response.hasKnownHeader(HttpResponse::kKnownHeaderAccessControlMaxAge)) {
        return;
    }
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), maxAge.count());
    if (ec == std::errc{}) {
        response.setHeader("Access-Control-Max-Age", std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
}

void applyCorsHeaders(const HttpRequest& request, HttpResponse& response, const HttpServerOptions::Cors& cors) {
    if (!cors.enabled) {
        return;
    }

    const auto origin = request.header(HttpRequest::KnownHeader::kOrigin);
    if (origin.empty()) {
        return;
    }

    const auto configuredOrigin = std::string_view(cors.allowOrigin);
    const auto allowOrigin = configuredOrigin == "*" && cors.allowCredentials ? origin : configuredOrigin;
    setHeaderIfMissing(
        response,
        HttpResponse::kKnownHeaderAccessControlAllowOrigin,
        "Access-Control-Allow-Origin",
        allowOrigin);
    if (allowOrigin != "*") {
        addVaryToken(response, "Origin");
    }
    if (cors.allowCredentials) {
        setHeaderIfMissing(
            response,
            HttpResponse::kKnownHeaderAccessControlAllowCredentials,
            "Access-Control-Allow-Credentials",
            "true");
    }

    const bool preflight = request.method() == HttpMethod::kOptions &&
        !request.header(HttpRequest::KnownHeader::kAccessControlRequestMethod).empty();
    if (preflight) {
        if (const auto allow = response.header(HttpResponse::kKnownHeaderAllow); !allow.empty()) {
            setHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowMethods,
                "Access-Control-Allow-Methods",
                allow);
        }
        const auto configuredHeaders = std::string_view(cors.allowHeaders);
        const auto requestedHeaders = request.header(HttpRequest::KnownHeader::kAccessControlRequestHeaders);
        if (!configuredHeaders.empty()) {
            setHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                configuredHeaders);
        } else if (!requestedHeaders.empty()) {
            setHeaderIfMissing(
                response,
                HttpResponse::kKnownHeaderAccessControlAllowHeaders,
                "Access-Control-Allow-Headers",
                requestedHeaders);
            addVaryToken(response, "Access-Control-Request-Headers");
        }
        addVaryToken(response, "Access-Control-Request-Method");
        setCorsMaxAge(response, cors.maxAge);
        return;
    }

    if (!cors.exposeHeaders.empty()) {
        setHeaderIfMissing(
            response,
            HttpResponse::kKnownHeaderAccessControlExposeHeaders,
            "Access-Control-Expose-Headers",
            cors.exposeHeaders);
    }
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

void compactConnectionReadBuffer(
    std::pmr::string& readBuffer,
    std::size_t& usedBytes,
    std::size_t consumedBytes) noexcept {
    const auto remainingBytes = usedBytes - consumedBytes;
    if (remainingBytes > 0) {
        std::memmove(readBuffer.data(), readBuffer.data() + consumedBytes, remainingBytes);
    }
    usedBytes = remainingBytes;
}

template <typename Stream>
Task<bool> writeWebSocketHandshake(
    Stream& stream,
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supportedSubprotocols,
    std::pmr::memory_resource* resource) {
    auto accept = webSocketAccept(request.header(HttpRequest::KnownHeader::kSecWebSocketKey), resource);
    const auto subprotocol = chooseWebSocketSubprotocol(request, flags, supportedSubprotocols);
    std::pmr::string response(resource);
    response.append("HTTP/1.1 101 Switching Protocols\r\n");
    response.append("Upgrade: websocket\r\n");
    response.append("Connection: Upgrade\r\n");
    response.append("Sec-WebSocket-Accept: ");
    response.append(accept);
    response.append("\r\n");
    if (!subprotocol.empty()) {
        response.append("Sec-WebSocket-Protocol: ");
        response.append(subprotocol);
        response.append("\r\n");
    }
    response.append("\r\n");
    const auto ec = co_await asyncError([&stream, view = std::string_view(response)](auto handler) mutable {
        asio::async_write(stream, asio::buffer(view), std::move(handler));
    });
    co_return !ec;
}

void trimReadBufferStorage(std::pmr::string& readBuffer, std::size_t usedBytes) {
    if (usedBytes > kInitialReadBufferBytes) {
        return;
    }

    if (readBuffer.capacity() > kReadBufferShrinkCapacityBytes) {
        std::pmr::string compact(readBuffer.get_allocator());
        compact.resize(kInitialReadBufferBytes);
        if (usedBytes > 0) {
            std::memcpy(compact.data(), readBuffer.data(), usedBytes);
        }
        readBuffer = std::move(compact);
        return;
    }

    if (readBuffer.size() < kInitialReadBufferBytes) {
        readBuffer.resize(kInitialReadBufferBytes);
        return;
    }

    if (readBuffer.size() > kInitialReadBufferBytes) {
        readBuffer.resize(kInitialReadBufferBytes);
    }
}

void growReadBuffer(std::pmr::string& readBuffer, std::size_t usedBytes, const HttpParseResult& parsed) {
    if (parsed.consumedBytes > readBuffer.size()) {
        readBuffer.resize(parsed.consumedBytes);
        return;
    }

    if (usedBytes == readBuffer.size() && readBuffer.size() < kMaxHttpHeaderBytes) {
        readBuffer.resize(std::min(readBuffer.size() * 2, kMaxHttpHeaderBytes));
    }
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

}  // namespace

struct ConnectionState final {
    explicit ConnectionState(WorkerMemory& memory)
        : readBuffer(memory.allocator<char>()),
          responseHead(memory.allocator<char>()),
          fileChunk(memory.allocator<char>()) {
        readBuffer.resize(kInitialReadBufferBytes);
    }

    ConnectionScanner::Entry scannerEntry;
    std::pmr::string readBuffer;
    ResponseHeadBuffer responseHead;
    std::pmr::vector<char> fileChunk;
    std::size_t usedBytes{0};
    std::size_t requestCount{0};
    HttpParser parser;
};

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    HttpServerOptions options)
    : HttpServer(std::move(endpoint), routes, databases, std::span<const RedisDefinition>{}, options) {}

HttpServer::HttpServer(
    TcpEndpoint endpoint,
    const RouteTable& routes,
    std::span<const DbDefinition> databases,
    std::span<const RedisDefinition> redis,
    HttpServerOptions options)
    // One worker thread runs all I/O on this context; cross-thread access is
    // limited to stop()'s asio::post, which UNSAFE_IO keeps locked. Only the
    // reactor's per-descriptor I/O locking is elided.
    : ioContext_(ASIO_CONCURRENCY_HINT_UNSAFE_IO),
      acceptor_(ioContext_),
      endpoint_(std::move(endpoint)),
      routes_(routes),
      options_(options),
      databases_(ioContext_, memory_.resource(), databases),
      redis_(ioContext_, memory_.resource(), redis),
      connectionScanner_(std::make_unique<ConnectionScanner>(ioContext_.get_executor(), options_)) {
    if (databases_.hasAnyTimeout()) {
        connectionScanner_->setWorkerScanner(&databases_, [](void* target) noexcept {
            static_cast<DbRegistry*>(target)->scanDeadlines();
        });
    }
    if (redis_.hasAnyTimeout()) {
        connectionScanner_->setWorkerScanner(&redis_, [](void* target) noexcept {
            static_cast<RedisRegistry*>(target)->scanDeadlines();
        });
    }
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true)) {
        throw std::logic_error("http server worker is already started");
    }

    try {
        resetStartupState();
        configureAcceptor();
        configureTlsContext();
        connectionScanner_->start();
        asio::co_spawn(
            ioContext_,
            taskAsAwaitable(runWorker()),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
        workerThread_ = std::jthread([this] { runIoContext(); });
        waitForStartupReady();
    } catch (...) {
        started_ = false;
        if (workerThread_.joinable()) {
            workerThread_.join();
        } else {
            stopOnContext();
        }
        throw;
    }
}

void HttpServer::stop() {
    if (!started_.exchange(false)) {
        return;
    }

    asio::post(ioContext_, [this] { stopOnContext(); });
}

void HttpServer::join() {
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

TcpEndpoint HttpServer::localEndpoint() const {
    return endpoint_;
}

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

void HttpServer::configureAcceptor() {
    std::error_code ec;

    acceptor_.open(endpoint_.protocol(), ec);
    if (ec) {
        throw std::runtime_error("failed to open acceptor: " + ec.message());
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("failed to enable SO_REUSEADDR: " + ec.message());
    }

#if defined(SO_REUSEPORT) && !defined(_WIN32)
    int enabled = 1;
    if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to enable SO_REUSEPORT");
    }
#elif !defined(_WIN32)
    throw std::runtime_error("SO_REUSEPORT is required but not available on this platform/toolchain");
#endif

    acceptor_.bind(endpoint_, ec);
    if (ec) {
        throw std::runtime_error("failed to bind acceptor: " + ec.message());
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("failed to listen: " + ec.message());
    }

    endpoint_ = acceptor_.local_endpoint(ec);
    if (ec) {
        throw std::runtime_error("failed to read local endpoint: " + ec.message());
    }
}

void HttpServer::configureTlsContext() {
    if (!options_.tls.enabled) {
        tlsContext_.reset();
        return;
    }
    if (options_.tls.certificateChainFile.empty() || options_.tls.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS requires certificate chain and private key files");
    }

    tlsContext_.emplace(asio::ssl::context::tls_server);
    auto& context = *tlsContext_;
    context.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 |
        asio::ssl::context::no_tlsv1 |
        asio::ssl::context::no_tlsv1_1 |
        asio::ssl::context::single_dh_use);
    SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);

    if (!options_.tls.privateKeyPassword.empty()) {
        context.set_password_callback([password = options_.tls.privateKeyPassword](std::size_t, asio::ssl::context::password_purpose) {
            return std::string(password);
        });
    }
    context.use_certificate_chain_file(options_.tls.certificateChainFile.string());
    context.use_private_key_file(options_.tls.privateKeyFile.string(), asio::ssl::context::pem);
    if (!options_.tls.verifyFile.empty()) {
        context.load_verify_file(options_.tls.verifyFile.string());
        context.set_verify_mode(asio::ssl::verify_peer);
    }
}

void HttpServer::stopOnContext() noexcept {
    std::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);

    if (connectionScanner_ != nullptr) {
        connectionScanner_->stop();
        connectionScanner_->closeAll();
    }

    databases_.closeNow();
    redis_.closeNow();
}

void HttpServer::resetStartupState() {
    std::lock_guard lock(startupMutex_);
    startupException_ = nullptr;
    startupReady_ = false;
}

void HttpServer::completeStartup(std::exception_ptr exception) noexcept {
    {
        std::lock_guard lock(startupMutex_);
        if (startupReady_) {
            return;
        }

        startupException_ = exception;
        startupReady_ = true;
    }
    startupCv_.notify_all();
}

void HttpServer::waitForStartupReady() {
    std::unique_lock lock(startupMutex_);
    startupCv_.wait(lock, [this] { return startupReady_; });
    if (startupException_ != nullptr) {
        std::rethrow_exception(startupException_);
    }
}

void HttpServer::runIoContext() noexcept {
    try {
        ioContext_.run();
    } catch (...) {
        started_.store(false, std::memory_order_relaxed);
        stopOnContext();
        completeStartup(std::current_exception());
        return;
    }

    completeStartup(std::make_exception_ptr(
        std::runtime_error("http server worker stopped before startup completed")));
}

Task<void> HttpServer::runWorker() {
    try {
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
        completeStartup();
        co_await acceptLoop();
    } catch (...) {
        started_.store(false, std::memory_order_relaxed);
        stopOnContext();
        completeStartup(std::current_exception());
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
            // bad accept must not stop the worker forever — back off briefly
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
        // Socket state may be partially written or completely fine — we
        // cannot safely emit anything new, so just drop the connection.
        closeSocket(socket);
    }
}

template <typename Stream>
Task<void> HttpServer::handleStreamSession(Stream& stream, TcpSocket& socket) {
    ConnectionState connection(memory_);
    ConnectionScanner::Guard scannerGuard(connectionScanner_.get(), connection.scannerEntry, socket);
    auto& scannerEntry = connection.scannerEntry;
    auto& readBuffer = connection.readBuffer;
    auto& usedBytes = connection.usedBytes;
    auto& requestCount = connection.requestCount;
    auto& parser = connection.parser;
    const auto& routes = routes_;
    std::pmr::string remoteAddress(memory_.allocator<char>());
    std::error_code remoteEc;
    const auto remoteEndpoint = socket.remote_endpoint(remoteEc);
    if (!remoteEc) {
        remoteAddress = remoteEndpoint.address().to_string();
    }

    // One connection-scoped parse result, reset in place by parseHeaders():
    // the ~2.5KB struct is never copied or re-zeroed per request/read.
    HttpParseResult parsed;

    for (;;) {
        scannerEntry.setPhase(ConnectionScanner::Phase::kIdle);
        std::array<std::byte, kRequestArenaStackBytes> requestArenaBuffer;
        std::optional<RequestMemory> requestMemoryStorage;
        if (memory_.requestInitialBufferBytes() <= requestArenaBuffer.size()) {
            requestMemoryStorage.emplace(
                memory_,
                std::span<std::byte>(requestArenaBuffer.data(), memory_.requestInitialBufferBytes()));
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
        RouteResolution routeResolution;
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
                        // concurrent write active) — swallow that too.
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
                        connection.responseHead,
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
                &connection.responseHead,
                &connection.fileChunk,
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
